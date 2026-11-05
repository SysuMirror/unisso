<template>
  <div class="developer-page">
    <div class="page-head">
      <div>
        <h1 class="page-title">开放平台</h1>
        <p class="page-sub">
          UniSSO 统一身份认证为校内各平台与第三方应用提供登录能力 · 无需登录即可查阅
        </p>
      </div>
      <div class="page-actions">
        <RouterLink to="/docs" class="btn btn-ghost btn-sm">接入文档</RouterLink>
        <button class="btn btn-primary btn-sm" type="button" @click="apply">申请接入</button>
      </div>
    </div>

    <div class="feature-grid">
      <div class="feature">
        <span class="feature-num">01</span>
        <h3>第三方平台（跳转授权）</h3>
        <p>
          标准 OAuth2 授权码 + PKCE(S256)：用户跳转到 UniSSO 登录并<b>逐次确认</b>授权范围，
          回调带回一次性 <code>code</code>，平台后端换取 token 与用户信息。
        </p>
        <div class="feature-links">
          <RouterLink to="/docs">查看接入文档 →</RouterLink>
        </div>
      </div>
      <div class="feature">
        <span class="feature-num">02</span>
        <h3>校内平台（无感登录）</h3>
        <p>
          同域校内系统共享登录态，可直接校验会话或由管理员开通 M2M 通道，
          免去重复登录。此方式仅对内部应用开放，需管理员开通。
        </p>
        <div class="feature-links">
          <span class="muted-text">由平台管理员开通</span>
        </div>
      </div>
      <div class="feature">
        <span class="feature-num">03</span>
        <h3>只要身份断言</h3>
        <p>
          不接入登录流程也可以依赖标准 OIDC 元数据：发现文档与 JWKS 公钥均为公开只读端点，
          可直接用于校验 RS256 签名的 ID Token。
        </p>
        <div class="feature-links">
          <a :href="discoveryUrl" target="_blank" rel="noopener noreferrer">Discovery →</a>
          <a :href="jwksUrl" target="_blank" rel="noopener noreferrer">JWKS →</a>
        </div>
      </div>
    </div>

    <div v-if="loadError" class="alert alert-warning meta-alert">
      平台元数据暂时不可用（{{ loadError }}），以下端点以实际部署为准。
    </div>

    <section class="section-block">
      <div class="section-head">
        <div>
          <h2 class="section-title">端点速查</h2>
          <p class="section-sub">所有端点均在 <code class="code-inline">{{ issuer }}</code> 之下。</p>
        </div>
      </div>
      <div class="table-wrap">
        <table class="table">
          <thead>
            <tr><th>用途</th><th>方法</th><th>端点</th></tr>
          </thead>
          <tbody>
            <tr v-for="row in endpointRows" :key="row.key">
              <td>{{ row.label }}</td>
              <td>{{ row.method }}</td>
              <td><span class="mono">{{ row.url }}</span></td>
            </tr>
          </tbody>
        </table>
      </div>
      <p class="foot-note">
        授权码流程支持的 grant：<code class="code-inline">authorization_code</code>
        <code class="code-inline">refresh_token</code>；PKCE 方式：
        <code class="code-inline">S256</code>。
      </p>
    </section>

    <section class="section-block">
      <div class="section-head">
        <div>
          <h2 class="section-title">权限范围（scope）</h2>
          <p class="section-sub">
            实际授予 = 应用申请 ∩ 应用登记范围，且强制包含
            <code class="code-inline">openid</code>；登记范围须在上表白名单内。
          </p>
        </div>
      </div>
      <div class="table-wrap">
        <table class="table">
          <thead>
            <tr><th>scope</th><th>含义</th></tr>
          </thead>
          <tbody>
            <tr v-for="item in scopeRows" :key="item.name">
              <td><code class="code-inline">{{ item.name }}</code></td>
              <td>{{ item.description }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <section class="section-block">
      <div class="section-head">
        <div>
          <h2 class="section-title">错误契约</h2>
          <p class="section-sub">授权请求参数不合法时返回 JSON 400（不重定向）；回调错误通过 <code class="code-inline">error</code> 参数带回。</p>
        </div>
      </div>
      <div class="table-wrap">
        <table class="table">
          <thead>
            <tr><th>错误</th><th>出现位置</th><th>含义</th></tr>
          </thead>
          <tbody>
            <tr v-for="row in errorRows" :key="row.code">
              <td><code class="code-inline">{{ row.code }}</code></td>
              <td>{{ row.where }}</td>
              <td>{{ row.meaning }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <section class="section-block">
      <div class="section-head">
        <div>
          <h2 class="section-title">快速开始</h2>
          <p class="section-sub">生成 PKCE、跳转授权、后端换取 token 的最小流程。</p>
        </div>
      </div>
      <pre class="code-block"><code>{{ quickStart }}</code></pre>
    </section>

    <div class="cta panel panel-pad">
      <div>
        <h3>准备好了？</h3>
        <p>
          {{
            store.user
              ? '在个人中心提交接入申请，管理员审核通过后会发放 client_id / client_secret。'
              : '登录后即可在个人中心提交接入申请，管理员审核通过后发放接入凭据。'
          }}
        </p>
      </div>
      <button class="btn btn-primary" type="button" @click="apply">
        {{ store.user ? '去申请接入' : '登录并申请' }}
      </button>
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { api } from '../api'
import { store } from '../store'

const router = useRouter()
const discoveryUrl = '/.well-known/openid-configuration'
const jwksUrl = '/.well-known/jwks.json'

const meta = ref(null)
const loadError = ref('')

const ENDPOINT_LABELS = {
  authorize: { label: '授权入口（前端路由）', method: 'GET/302' },
  token: { label: '换取 / 刷新 token', method: 'POST' },
  userinfo: { label: '用户信息', method: 'GET' },
  introspect: { label: '客户端 / token 自检', method: 'POST' },
  discovery: { label: 'OIDC 发现文档', method: 'GET' },
  jwks: { label: 'JWKS 公钥', method: 'GET' }
}

const FALLBACK_ENDPOINTS = {
  authorize: '/authorize',
  token: '/api/oauth/token',
  userinfo: '/api/oauth/userinfo',
  introspect: '/api/oauth/introspect',
  discovery: '/.well-known/openid-configuration',
  jwks: '/.well-known/jwks.json'
}

const FALLBACK_SCOPES = [
  { name: 'openid', description: '基础身份标识（用户唯一 ID）' },
  { name: 'profile:username', description: '用户昵称' },
  { name: 'profile:name', description: '真实姓名' },
  { name: 'profile:avatar', description: '头像' },
  { name: 'profile:student_id', description: '学号' },
  { name: 'email', description: '邮箱地址' }
]

const issuer = computed(() => meta.value?.issuer || 'https://sso.ssemarket.cn')

const endpointRows = computed(() => {
  const endpoints = meta.value?.endpoints || {}
  return Object.keys(ENDPOINT_LABELS).map((key) => ({
    key,
    ...ENDPOINT_LABELS[key],
    url: endpoints[key] || `${issuer.value}${FALLBACK_ENDPOINTS[key]}`
  }))
})

const scopeRows = computed(() => {
  const catalog = meta.value?.scope_catalog
  return Array.isArray(catalog) && catalog.length ? catalog : FALLBACK_SCOPES
})

const errorRows = [
  { code: 'invalid_request', where: '/authorize', meaning: '缺少或非法参数' },
  { code: 'invalid_client', where: '/authorize、/api/oauth/token', meaning: 'client_id 或 secret 不正确' },
  { code: 'invalid_redirect_uri', where: '/authorize', meaning: '回调地址未登记（需逐字符一致）' },
  { code: 'unsupported_response_type', where: '/authorize', meaning: 'response_type 不是 code' },
  { code: 'invalid_scope', where: '/authorize、/api/apps', meaning: 'scope 不在白名单内' },
  { code: 'invalid_grant', where: '/api/oauth/token', meaning: 'code 缺失、过期或已被使用' },
  { code: 'access_denied', where: '回调参数', meaning: '用户在同意页选择拒绝' },
  { code: 'invalid_token', where: '/api/oauth/userinfo', meaning: 'access_token 无效或过期' }
]

const quickStart = `# 1) PKCE：code_challenge = base64url(sha256(verifier))，去掉 padding，恰好 43 字符
verifier  = <43-128 位随机串 [A-Za-z0-9-._~]>
challenge = BASE64URL(SHA256(verifier))
state     = <自己生成，回调时校验>

# 2) 浏览器 302 到授权入口（未登录会先跳登录，随后进同意页）
GET https://sso.ssemarket.cn/authorize
  ?client_id=<client_id>&redirect_uri=<登记的回调地址>
  &response_type=code&scope=openid profile:name email
  &state=<state>&code_challenge=<challenge>&code_challenge_method=S256

# 3) 回调收到 code 后，后端换取 token（client_secret 只留在后端）
POST https://sso.ssemarket.cn/api/oauth/token
  grant_type=authorization_code&code=<code>&redirect_uri=<同上>
  &client_id=<client_id>&client_secret=<secret>&code_verifier=<verifier>

# 4) 取用户信息
GET https://sso.ssemarket.cn/api/oauth/userinfo
  Authorization: Bearer <access_token>`

function apply() {
  if (store.user) router.push('/profile')
  else router.push({ path: '/login', query: { next: '/developer' } })
}

onMounted(async () => {
  try {
    meta.value = await api('/api/public/docs')
  } catch (err) {
    loadError.value = err.message || '加载失败'
  }
})
</script>

<style scoped>
.page-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.feature p { font-size: 13px; line-height: 1.8; }
.feature-links {
  display: flex;
  flex-wrap: wrap;
  gap: 14px;
  margin-top: 12px;
  font-size: 12.5px;
}
.feature-links a { color: var(--brand-ink); }
.muted-text { color: var(--ink-faint); }
.meta-alert { margin: 24px 0 0; font-size: 13px; }
.section-block { margin-top: 40px; }
.section-head { margin-bottom: 14px; }
.foot-note {
  margin-top: 10px;
  font-size: 12.5px;
  color: var(--ink-soft);
}
.mono { font-family: var(--mono); font-size: 12px; word-break: break-all; }
.code-block {
  margin: 0;
  padding: 16px 18px;
  border-radius: var(--radius-sm);
  background: #fafafa;
  border: 1px solid var(--line);
  overflow-x: auto;
}
.code-block code {
  font-family: var(--mono);
  font-size: 12px;
  line-height: 1.75;
  color: var(--ink);
  white-space: pre;
}
.cta {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 20px;
  margin-top: 40px;
}
.cta h3 { font-size: 15px; font-weight: 600; }
.cta p {
  margin-top: 6px;
  font-size: 13px;
  line-height: 1.8;
  color: var(--ink-soft);
}

@media (max-width: 720px) {
  .cta { flex-direction: column; align-items: flex-start; }
  .cta .btn { width: 100%; }
}
</style>
