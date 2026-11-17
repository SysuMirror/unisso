#!/usr/bin/env python3
"""Run real HTTP qualification tests with private, disposable services on Linux.

Requires a built UniSSO binary, mariadbd, mariadb, redis-server, redis-cli,
openssl and Python 3. Uses loopback ports and a new temporary data directory;
does not read deployment credentials or connect to existing services.

python3 backend/tests/qualification/run.py --binary backend/build/unisso_v2
Use --deps-root for dependencies extracted under a private prefix.
"""
import argparse
import base64
import ctypes
import ctypes.util
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def wait_for(check, description):
    until = time.monotonic() + 20
    while time.monotonic() < until:
        if check():
            return
        time.sleep(0.1)
    raise RuntimeError('Timed out waiting for ' + description)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--deps-root', type=Path)
    parser.add_argument('--baseline', action='store_true', help='Record old behavior without expecting fixes')
    parser.add_argument('--results', type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    root = Path(tempfile.mkdtemp(prefix='unisso-qualification-', dir='/var/tmp'))
    print('Private test directory:', root, flush=True)
    env = os.environ.copy()
    # Explicitly discard inherited deployment configuration and proxies.
    for key in list(env):
        if key.startswith(('MYSQL_', 'REDIS_', 'UNISSO_', 'SSE_', 'SMTP_', 'REPORT_')) or key.lower().endswith('_proxy'):
            env.pop(key)
    if args.deps_root:
        deps = args.deps_root.resolve()
        env['LD_LIBRARY_PATH'] = str(deps/'usr/lib/x86_64-linux-gnu')
        env['PATH'] = str(deps/'usr/sbin') + ':' + str(deps/'usr/bin') + ':' + env['PATH']
        basedir = deps/'usr'
    else:
        basedir = Path('/usr')

    def executable(name):
        found = shutil.which(name, path=env['PATH'])
        if not found:
            raise RuntimeError('Missing executable: ' + name)
        return found

    db_port, redis_port, http_port = port(), port(), port()
    dbdir = root/'mysql'
    dbdir.mkdir()
    (dbdir/'mysql').mkdir()
    processes = {}
    logs = []
    results = []
    completed = False
    http = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def launch(name, command, process_env=env):
        log = (root/(name+'.log')).open('ab')
        logs.append(log)
        processes[name] = subprocess.Popen(command, cwd=root, env=process_env, stdout=log, stderr=subprocess.STDOUT)

    def stop(name):
        proc = processes.pop(name, None)
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    mysql = executable('mariadb')
    mysqld = executable('mariadbd')
    redis_server = executable('redis-server')
    redis_cli = executable('redis-cli')

    def sql(statement):
        return subprocess.run([mysql, '--no-defaults', '--protocol=TCP', '--host=127.0.0.1',
            '--port='+str(db_port), '--user=root', '--batch', '--skip-column-names'],
            input=statement, text=True, env=env, check=True, capture_output=True).stdout.strip()

    def db_ready():
        try:
            return sql('SELECT 1') == '1'
        except subprocess.CalledProcessError:
            return False

    def start_db():
        launch('mysql', [mysqld, '--no-defaults', '--basedir='+str(basedir), '--datadir='+str(dbdir),
            '--socket='+str(root/'mysql.sock'), '--pid-file='+str(root/'mysql.pid'),
            '--port='+str(db_port), '--bind-address=127.0.0.1', '--skip-grant-tables',
            '--skip-log-bin', '--user='+str(os.getuid()), '--innodb-buffer-pool-size=32M'])
        wait_for(db_ready, 'private SQL server')

    def redis(*command):
        return subprocess.run([redis_cli, '-h', '127.0.0.1', '-p', str(redis_port), '--raw', *command],
            env=env, text=True, capture_output=True, check=True).stdout.strip()

    def start_redis():
        launch('redis', [redis_server, '--port', str(redis_port), '--bind', '127.0.0.1',
            '--dir', str(root), '--save', '', '--appendonly', 'no'])
        def ready():
            try:
                return redis('PING') == 'PONG'
            except subprocess.CalledProcessError:
                return False
        wait_for(ready, 'private Redis')

    def request(endpoint, data=None, headers=None):
        body = json.dumps(data).encode() if data is not None else None
        req = urllib.request.Request('http://127.0.0.1:'+str(http_port)+endpoint,
            data=body, headers={'Content-Type':'application/json', **(headers or {})})
        try:
            with http.open(req, timeout=8) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def expect(name, actual, status=200, error=None, developer=None):
        code, data = actual
        passed = code == status and (error is None or data.get('error') == error)
        if developer is not None:
            passed = passed and data.get('user', data).get('developer') is developer
        results.append({'name':name, 'passed':passed, 'status':code,
            'error':data.get('error'), 'developer':data.get('user', data).get('developer')})
        print(('PASS ' if passed else 'FAIL ')+name+': '+str(code), flush=True)
        if not passed:
            raise AssertionError(name+': '+json.dumps(data, ensure_ascii=False))
        return data

    secret = 'test-only-client-secret'
    credentials = lambda client: {'client_id':client, 'client_secret':secret}
    update = lambda client='join-test', **fields: request('/api/oauth/developer/update', {**credentials(client), **fields})
    session = None
    try:
        start_db()
        start_redis()
        sql((Path(__file__).parent/'schema.sql').read_text())
        # Hash fixture passwords with the same system crypt implementation as C++.
        crypt = ctypes.CDLL(ctypes.util.find_library('crypt'))
        crypt.crypt.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        crypt.crypt.restype = ctypes.c_char_p
        password = 'qualification-test-password'
        hashed = crypt.crypt(password.encode(), b'$2b$04$abcdefghijklmnopqrstuu').decode()
        client_hash = crypt.crypt(secret.encode(), b'$2b$04$abcdefghijklmnopqrstuu').decode()
        sql("USE qualification; INSERT INTO users (id,email,password_hash,username,sse_user_id) VALUES "
            "('test-user','test@example.invalid','"+hashed+"','tester','market-test');"
            "INSERT INTO applications (id,client_id,client_secret) VALUES "+','.join(
                "('"+client+"','"+client+"','"+client_hash+"')" for client in
                ['join-test','develop-test','public-test','external-test','disabled-test','no-m2m-test','pending-test'])+';'
            "UPDATE applications SET is_confidential=0 WHERE client_id='public-test';"
            "UPDATE applications SET kind='external' WHERE client_id='external-test';"
            "UPDATE applications SET is_active=0 WHERE client_id='disabled-test';"
            "UPDATE applications SET m2m_enabled=0 WHERE client_id='no-m2m-test';"
            "UPDATE applications SET status='pending' WHERE client_id='pending-test';")
        private_key = root/'private.pem'
        public_key = root/'public.pem'
        subprocess.run(['openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',str(private_key)],
            check=True, capture_output=True)
        subprocess.run(['openssl','pkey','-in',str(private_key),'-pubout','-out',str(public_key)],check=True,capture_output=True)
        env.update({'PORT':str(http_port), 'MYSQL_HOST':'127.0.0.1', 'MYSQL_PORT':str(db_port),
            'MYSQL_DB':'qualification', 'MYSQL_USER':'root', 'MYSQL_PASSWORD':'',
            'REDIS_HOST':'127.0.0.1', 'REDIS_PORT':str(redis_port), 'REDIS_PREFIX':'qualification',
            'REDIS_USER':'', 'REDIS_PASSWORD':'', 'UNISSO_CONFIG_FILE':str(root/'absent.yml'),
            'UNISSO_LOCAL_CONFIG_FILE':str(root/'absent.local.yml'),
            'UNISSO_SIGNING_PRIVATE_KEY_FILE':str(private_key), 'UNISSO_SIGNING_PUBLIC_KEY_FILE':str(public_key),
            'UNISSO_SIGNING_KEY_ID':'qualification-test',
            'UNISSO_SECRET_KEY':'test-only-signing-secret', 'UNISSO_DATA_ROOT':str(root/'data'),
            'UNISSO_WEB_ROOT':str(root/'web'), 'UNISSO_ISSUER':'http://127.0.0.1:'+str(http_port),
            'PUBLIC_URL':'http://127.0.0.1:'+str(http_port), 'REPORT_URL':'', 'SSE_ENABLED':'false',
            'UNISSO_SSE_SUPER_ADMIN_IDS':'-1',
            'UNISSO_DEVELOPER_WRITE_CLIENTS':'join-test, public-test, external-test, disabled-test, no-m2m-test, pending-test',
            'UNISSO_PASSWORD_VERIFY_CLIENTS':'external-test'})
        launch('unisso', [str(binary)])

        def backend_ready():
            if processes['unisso'].poll() is not None:
                raise RuntimeError('UniSSO exited; see '+str(root/'unisso.log'))
            try:
                return request('/api/health')[0] == 200
            except (urllib.error.URLError, ConnectionError):
                return False
        wait_for(backend_ready, 'UniSSO HTTP')
        # A signed fixture token exercises userinfo; token issuance/PKCE are
        # outside this test's scope.
        b64 = lambda raw: base64.urlsafe_b64encode(raw).rstrip(b'=')
        now = int(time.time())
        claims = {'sub':'test-user','type':'access','iat':now,'exp':now+600,
            'jti':'fixture-token','iss':env['UNISSO_ISSUER'],
            'aud':env['UNISSO_ISSUER']+'/api/oauth/userinfo',
            'client_id':'develop-test','scope':'openid'}
        signing = b64(b'{"alg":"RS256","typ":"JWT","kid":"qualification-test"}')+b'.'+b64(json.dumps(claims).encode())
        signature = subprocess.run(['openssl','dgst','-sha256','-sign',str(private_key)],
            input=signing,check=True,capture_output=True).stdout
        token = (signing+b'.'+b64(signature)).decode()
        sql("USE qualification; INSERT INTO access_tokens (id,token) VALUES ('fixture-token','fixture-token')")
        userinfo = lambda: request('/api/oauth/userinfo',headers={'Authorization':'Bearer '+token})
        login = lambda: request('/api/oauth/password/verify', {**credentials('develop-test'), 'email':'test@example.invalid','password':password})
        session = expect('login without qualification', login(), developer=False)['sso_session']
        read = lambda: request('/api/oauth/session/verify', {**credentials('develop-test'), 'session':session})
        # Simulated downstream guard consumes the real identity response.
        def develop_access():
            code, data = read()
            if code != 200:
                return code, data
            if data['user'].get('developer') is not True:
                return 403, {'error':'developer_required'}
            return 200, {'ok':True}
        expect('Develop reads unqualified identity', read(), developer=False)
        expect('simulated Develop denies protected business',develop_access(),403,'developer_required')
        expect('userinfo reads initial qualification',userinfo(),developer=False)
        expect('Join grants qualification', update(user_id='test-user',is_developer=True), developer=True)
        expect('existing Develop session sees grant', read(), developer=True)
        expect('simulated Develop allows protected business',develop_access())
        expect('same access token sees grant',userinfo(),developer=True)
        expect('duplicate grant', update(user_id='test-user',is_developer=True), developer=True)
        expect('Join revokes qualification', update(user_id='test-user',is_developer=False), developer=False)
        expect('existing Develop session sees revocation', read(), developer=False)
        expect('simulated Develop denies after revocation',develop_access(),403,'developer_required')
        expect('same access token sees revocation',userinfo(),developer=False)
        assert sql('USE qualification; SELECT COUNT(*) FROM users') == '1'

        if args.baseline:
            for name, actual in [
                ('missing flag', update(user_id='test-user')),
                ('Develop writes qualification', update('develop-test',user_id='test-user',is_developer=True)),
                ('public client without secret', request('/api/oauth/developer/update',{'client_id':'public-test','user_id':'test-user','is_developer':True}))]:
                results.append({'name':name,'observed_status':actual[0]})
                print('BASELINE',name,actual[0],flush=True)
            stop('redis')
            for name, actual in [('Redis down session',read()),('Redis down login',login())]:
                results.append({'name':name,'observed_status':actual[0]})
                print('BASELINE',name,actual[0],flush=True)
            start_redis()
            print('BASELINE Redis recovered login', login()[0], flush=True)
            stop('mysql')
            print('BASELINE SQL down update',update(user_id='test-user',is_developer=True)[0],flush=True)
            completed = True
            return

        expect('Develop cannot write',update('develop-test',user_id='test-user',is_developer=True),403,'developer_update_forbidden')
        basic = base64.b64encode(('join-test:'+secret).encode()).decode()
        expect('Basic client authentication',request('/api/oauth/developer/update',
            {'user_id':'test-user','is_developer':False}, {'Authorization':'Basic '+basic}),developer=False)
        expect('wrong secret',request('/api/oauth/developer/update',{'client_id':'join-test','client_secret':'wrong','user_id':'test-user','is_developer':True}),401,'invalid_client')
        expect('public client cannot use M2M',request('/api/oauth/developer/update',{'client_id':'public-test','user_id':'test-user','is_developer':True}),401,'invalid_client')
        for client in ['external-test','no-m2m-test','pending-test']:
            expect(client+' cannot write',update(client,user_id='test-user',is_developer=True),403,'developer_update_forbidden')
        expect('disabled client',update('disabled-test',user_id='test-user',is_developer=True),401,'invalid_client')
        expect('unknown client',update('unknown-client',user_id='test-user',is_developer=True),401,'invalid_client')
        expect('unknown user',update(user_id='unknown-user',is_developer=True),404,'user_not_found')
        expect('missing target',update(is_developer=True),400,'invalid_request')
        expect('missing flag',update(user_id='test-user'),400,'invalid_request')
        expect('invalid flag',update(user_id='test-user',is_developer='yes'),400,'invalid_request')
        expect('missing credentials',request('/api/oauth/developer/update',{'user_id':'test-user','is_developer':True}),401,'invalid_client')
        expect('missing login password',request('/api/oauth/password/verify',{**credentials('develop-test'),'email':'test@example.invalid'}),400,'invalid_request')
        expect('unknown login account',request('/api/oauth/password/verify',{**credentials('develop-test'),'email':'unknown@example.invalid','password':password}),401,'invalid_credentials')
        expect('missing session',request('/api/oauth/session/verify',credentials('develop-test')),400,'invalid_request')
        expect('unknown session',request('/api/oauth/session/verify',{**credentials('develop-test'),'session':'unknown'}),401,'invalid_session')
        assert sql('USE qualification; SELECT is_developer FROM users WHERE id=\'test-user\'') == '0'
        expect('email target and legacy flag',update(email='TEST@example.invalid',developer=True),developer=True)
        expect('market ID target',update(sse_user_id='market-test',is_developer=False),developer=False)
        # A real SQL write error must not return success or change the old value.
        sql("USE qualification; CREATE TRIGGER fail_write BEFORE UPDATE ON users FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='fixture write failure'")
        expect('SQL write failure',update(user_id='test-user',is_developer=True),503,'temporarily_unavailable')
        assert sql('USE qualification; SELECT is_developer FROM users WHERE id=\'test-user\'') == '0'
        sql('USE qualification; DROP TRIGGER fail_write')
        expect('write recovers',update(user_id='test-user',is_developer=True),developer=True)
        # Initial identity read works; after qualification changes, a scalar
        # subquery fails on two rows during the confirming identity read.
        sql('USE qualification; RENAME TABLE user_identities TO fixture_identities;'
            "INSERT INTO fixture_identities (id,user_id) VALUES ('test-identity','test-user');"
            'CREATE TABLE fixture_read_fault (value INT); INSERT INTO fixture_read_fault VALUES (0),(0);'
            'CREATE VIEW user_identities AS SELECT id,user_id,created_at,'
            "CASE WHEN (SELECT is_developer FROM users WHERE id='test-user')=0 "
            'THEN (SELECT value FROM fixture_read_fault) ELSE is_active END AS is_active FROM fixture_identities;')
        expect('write commits but confirming read fails',update(user_id='test-user',is_developer=False),503,'temporarily_unavailable')
        assert sql('USE qualification; SELECT is_developer FROM users WHERE id=\'test-user\'') == '0'
        sql('USE qualification; DROP VIEW user_identities;'
            'RENAME TABLE fixture_identities TO user_identities; DROP TABLE fixture_read_fault;')
        expect('retry after confirming read recovers',update(user_id='test-user',is_developer=True),developer=True)
        # Block only SET: reads and rate-limit increments still work.
        redis('ACL','SETUSER','default','-set','-setex')
        expect('session write failure during login',login(),503,'temporarily_unavailable')
        expect('session extension write failure',read(),503,'temporarily_unavailable')
        redis('ACL','SETUSER','default','+set','+setex')
        expect('same session survives write failure',read(),developer=True)
        redis('ACL','SETUSER','default','-del')
        expect('session revoke failure',request('/api/oauth/session/revoke',{**credentials('develop-test'),'session':session}),503,'temporarily_unavailable')
        redis('ACL','SETUSER','default','+del')
        expect('failed revoke preserves session',read(),developer=True)
        stop('mysql')
        expect('SQL down update',update(user_id='test-user',is_developer=False),503,'temporarily_unavailable')
        expect('SQL down session',read(),503,'temporarily_unavailable')
        expect('SQL down login',login(),503,'temporarily_unavailable')
        expect('SQL down userinfo',userinfo(),503,'temporarily_unavailable')
        start_db()
        expect('SQL recovers with same session',read(),developer=True)
        expect('SQL recovers with same token',userinfo(),developer=True)
        stop('redis')
        expect('Redis down session',read(),503,'temporarily_unavailable')
        expect('Redis down login',login(),503,'temporarily_unavailable')
        expect('Redis down revoke',request('/api/oauth/session/revoke',{**credentials('develop-test'),'session':session}),503,'temporarily_unavailable')
        expect('qualification write independent of Redis',update(user_id='test-user',is_developer=False),developer=False)
        start_redis()
        expect('lost Redis session is invalid',read(),401,'invalid_session')
        session = expect('Redis recovers without backend restart',login(),developer=False)['sso_session']
        expect('new session works',read(),developer=False)
        expect('revoke session',request('/api/oauth/session/revoke',{**credentials('develop-test'),'session':session}))
        expect('revoked session invalid',read(),401,'invalid_session')
        expect('revoke is idempotent',request('/api/oauth/session/revoke',{**credentials('develop-test'),'session':session}))
        stop('unisso')
        env.pop('UNISSO_DEVELOPER_WRITE_CLIENTS')
        launch('unisso',[str(binary)])
        wait_for(backend_ready,'UniSSO with no writer configuration')
        expect('empty writer list denies by default',update(user_id='test-user',is_developer=True),403,'developer_update_forbidden')
        expect('empty writer list preserves login',login(),developer=False)
        print('All',len(results),'checks passed.',flush=True)
        completed = True
    finally:
        for name in list(processes):
            stop(name)
        for log in logs:
            log.close()
        report = {'binary':str(binary),'baseline':args.baseline,'completed':completed,'work_directory':str(root),'results':results}
        if args.results:
            args.results.parent.mkdir(parents=True,exist_ok=True)
            args.results.write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
        print('Services stopped. Logs retained:',root,flush=True)


if __name__ == '__main__':
    main()
