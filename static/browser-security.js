/* One browser request path: prefix handling and session-bound CSRF. */
(() => {
  const prefix = document.querySelector('meta[name="unisso-root"]').content.replace(/\/$/, '');
  const nativeFetch = window.fetch.bind(window);
  const endpoint = prefix + '/api/csrf-token';
  const localUrl = value => {
    if (typeof value === 'string' && value.startsWith('/api/')) return prefix + value;
    return value;
  };
  let pendingToken = null;
  async function csrfToken() {
    if (pendingToken) return pendingToken;
    pendingToken = requestToken().finally(() => { pendingToken = null; });
    return pendingToken;
  }
  async function requestToken() {
    const r = await nativeFetch(endpoint, {credentials: 'same-origin', cache: 'no-store'});
    if (!r.ok) throw new Error('安全校验暂不可用，请刷新后重试');
    return (await r.json()).csrf_token;
  }
  window.fetch = async (input, options = {}) => {
    input = localUrl(input);
    const destination = new URL(typeof input === 'string' ? input : input.url, location.href);
    const method = (options.method || (input instanceof Request ? input.method : 'GET')).toUpperCase();
    if (destination.origin === location.origin && !['GET','HEAD','OPTIONS'].includes(method)) {
      const headers = new Headers(options.headers || (input instanceof Request ? input.headers : undefined));
      headers.set('X-CSRF-Token', await csrfToken());
      options = {...options, headers, credentials: 'same-origin'};
    }
    return nativeFetch(input, options);
  };
  const ready = new WeakSet();
  document.addEventListener('submit', async event => {
    const form = event.target;
    if (form.method.toLowerCase() !== 'post' || event.defaultPrevented || ready.has(form)) {
      ready.delete(form);
      return;
    }
    if (new URL(form.action, location.href).origin !== location.origin) return;
    event.preventDefault();
    const submitter = event.submitter;
    try {
      let field = form.querySelector('input[name="csrf_token"]');
      if (!field) {
        field = document.createElement('input'); field.type='hidden'; field.name='csrf_token'; form.append(field);
      }
      field.value = await csrfToken();
      ready.add(form);
      form.requestSubmit(submitter);
    } catch (error) { alert(error.message); }
  });
})();
