<template>
  <div class="consent-page">
    <div class="consent-decoration" aria-hidden="true">
      <span class="consent-orbit orbit-a"></span>
      <span class="consent-orbit orbit-b"></span>
      <img class="consent-watermark" :src="logo" alt="" />
    </div>

    <div class="consent-brand">
      <img :src="logo" alt="" />
      <span>统一身份认证</span>
    </div>

    <div v-if="loading" class="consent-panel consent-state">
      <span class="spinner"></span>
      正在校验授权请求…
    </div>

    <div v-else-if="error" class="consent-panel consent-state">
      <h1 class="consent-title">无法继续授权</h1>
      <p class="consent-request">{{ error }}</p>
      <RouterLink to="/" class="btn btn-secondary">返回首页</RouterLink>
    </div>

    <div v-else-if="context" class="consent-panel">
      <div class="consent-logos" aria-hidden="true">
        <span class="consent-logo">
          <img v-if="context.application.icon" :src="context.application.icon" alt="" />
          <span v-else class="consent-logo-fallback">{{ appInitial }}</span>
        </span>
        <span class="consent-link"></span>
        <span class="consent-logo consent-logo-sso">
          <img :src="logo" alt="" />
        </span>
      </div>

      <h1 class="consent-title">授权给 {{ context.application.name }}</h1>
      <p class="consent-request">
        {{ context.application.name }} 希望访问你的{{ scopeSummary.join('、') }}。
      </p>

      <div class="consent-account">
        <span class="consent-account-avatar">
          <img v-if="selectedAccount?.avatar" :src="selectedAccount.avatar" alt="" />
          <template v-else>{{ accountInitial }}</template>
        </span>
        <div class="consent-account-main">
          <div class="consent-account-name">{{ accountName }}</div>
          <div class="consent-account-email">{{ selectedAccount?.email }}</div>
        </div>
        <select
          v-if="accounts.length > 1"
          v-model="selectedAccountId"
          class="consent-account-select"
          aria-label="选择授权账号"
        >
          <option v-for="account in accounts" :key="account.id" :value="account.id">
            {{ account.full_name || account.username || account.email }}
          </option>
        </select>
      </div>

      <div v-if="sseIdentities.length > 1" class="consent-identities">
        <div class="consent-identities-label">以哪个集市身份进入？</div>
        <label v-for="identity in sseIdentities" :key="identity.id" class="consent-identity">
          <input v-model="selectedSseIdentity" type="radio" name="sse-identity" :value="identity.id" />
          <span class="consent-identity-main">
            <span class="consent-identity-name">{{ identity.name || identity.id }}</span>
            <span v-if="identity.email" class="consent-identity-email">{{ identity.email }}</span>
          </span>
          <span v-if="identity.is_current" class="consent-identity-tag">当前</span>
        </label>
      </div>

      <div v-if="submitError" class="alert alert-danger consent-error">{{ submitError }}</div>

      <button class="btn btn-primary consent-allow" type="button"
              :disabled="submitting" @click="decide(true)">
        {{ submitting ? '处理中…' : '允许' }}
      </button>
      <button class="consent-cancel" type="button"
              :disabled="submitting" @click="decide(false)">取消</button>

      <p class="consent-note">授权后可随时在“我的授权”中撤销。</p>
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { api } from '../api'
import logo from '../assets/logo.png'

const route = useRoute()
const router = useRouter()
const loading = ref(true)
const submitting = ref(false)
const error = ref('')
const submitError = ref('')
const context = ref(null)
const accounts = ref([])
const selectedAccountId = ref('')
const selectedSseIdentity = ref('')

const selectedAccount = computed(() =>
  accounts.value.find((account) => account.id === selectedAccountId.value) || accounts.value[0])
const sseIdentities = computed(() => selectedAccount.value?.sse_identities || [])

watch(selectedAccount, (account) => {
  const identities = account?.sse_identities || []
  const current = identities.find((identity) => identity.is_current)
  selectedSseIdentity.value = current ? current.id : (identities[0]?.id || '')
})
const accountName = computed(() => {
  const account = selectedAccount.value
  return account?.full_name || account?.username || account?.email || '当前账号'
})
const accountInitial = computed(() => (accountName.value || '?').slice(0, 1).toUpperCase())
const appInitial = computed(() => (context.value?.application?.name || '?').slice(0, 1).toUpperCase())

const scopeSummary = computed(() => {
  const names = new Set((context.value?.scopes || []).map((scope) => scope.name))
  const profile = []
  if (names.has('profile:avatar')) profile.push('头像')
  if (names.has('profile:username')) profile.push('昵称')
  if (names.has('profile:name')) profile.push('姓名')
  if (names.has('profile:student_id')) profile.push('学号')
  const parts = []
  if (profile.length) parts.push(`基本资料（${profile.join('、')}）`)
  if (names.has('email')) parts.push('邮箱地址')
  if (names.has('roles')) parts.push('角色信息')
  return parts.length ? parts : ['基础账号信息']
})

function requestParams() {
  const keys = ['client_id', 'redirect_uri', 'response_type', 'scope', 'state',
                'code_challenge', 'code_challenge_method', 'nonce']
  const params = new URLSearchParams()
  for (const key of keys) {
    const value = route.query[key]
    if (typeof value === 'string' && value) params.set(key, value)
  }
  return params
}

async function load() {
  loading.value = true
  error.value = ''
  try {
    const data = await api('/api/oauth/authorize/context?' + requestParams().toString())
    context.value = data
    accounts.value = data.accounts || []
    const preferred = accounts.value.find((account) => account.is_current) || accounts.value[0]
    selectedAccountId.value = preferred?.id || ''
  } catch (e) {
    if (e.status === 401) {
      router.push({ path: '/login', query: { next: route.fullPath } })
      return
    }
    error.value = e.message
  } finally {
    loading.value = false
  }
}

async function decide(approved) {
  if (!context.value) return
  submitError.value = ''
  submitting.value = true
  try {
    const request = context.value.request
    const data = await api('/api/oauth/authorize', {
      method: 'POST',
      body: {
        client_id: request.client_id,
        redirect_uri: request.redirect_uri,
        response_type: 'code',
        scope: request.scope,
        state: request.state,
        code_challenge: request.code_challenge,
        code_challenge_method: request.code_challenge_method,
        nonce: request.nonce,
        as_user_id: selectedAccountId.value,
        as_sse_identity: sseIdentities.value.length > 1 ? selectedSseIdentity.value : '',
        approved,
        scopes: approved ? (context.value.scopes || []).map((scope) => scope.name) : []
      }
    })
    if (data.redirect_url) window.location.assign(data.redirect_url)
  } catch (e) {
    submitError.value = e.message
    submitting.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.consent-page {
  position: relative;
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 64px 20px 40px;
  overflow-x: hidden;
  background-color: #fbfbfa;
  background-image:
    radial-gradient(900px 520px at 82% 18%, rgba(51, 112, 255, .07), transparent 65%),
    radial-gradient(700px 480px at 12% 88%, rgba(51, 112, 255, .045), transparent 70%),
    url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='48' height='48'%3E%3Cpath d='M24 20v8M20 24h8' stroke='%23d3d1ca' stroke-width='1'/%3E%3Cpath d='M0 24h48M24 0v48' stroke='%23eeede8' stroke-width='1'/%3E%3C/svg%3E");
  background-size: auto, auto, 48px 48px;
  background-position: 0 0, 0 0, 0 0;
}
.consent-decoration {
  position: absolute;
  right: 5vw;
  top: 50%;
  width: min(31vw, 390px);
  aspect-ratio: 1;
  transform: translateY(-50%);
  pointer-events: none;
  z-index: 0;
}
.consent-orbit {
  position: absolute;
  inset: 0;
  border: 1px solid rgba(51, 112, 255, .13);
  border-radius: 50%;
  animation: consent-spin 90s linear infinite;
}
.consent-orbit.orbit-b {
  inset: 14%;
  border-style: dashed;
  border-color: rgba(23, 23, 23, .08);
  animation-duration: 140s;
  animation-direction: reverse;
}
.consent-orbit::after {
  content: "";
  position: absolute;
  top: -3px;
  left: 50%;
  width: 5px;
  height: 5px;
  margin-left: -3px;
  border-radius: 50%;
  background: var(--brand);
  box-shadow: 0 0 0 4px rgba(51, 112, 255, .08);
}
.consent-orbit.orbit-b::after {
  width: 4px;
  height: 4px;
  background: var(--ink-faint);
  box-shadow: none;
}
.consent-watermark {
  position: absolute;
  inset: 31%;
  width: 38%;
  height: 38%;
  object-fit: contain;
  opacity: .07;
  animation: consent-spin 120s linear infinite reverse;
}
@keyframes consent-spin {
  to { transform: rotate(360deg); }
}
.consent-brand {
  position: absolute;
  top: 30px;
  left: 36px;
  display: flex;
  align-items: center;
  gap: 9px;
  z-index: 2;
  color: var(--ink-soft);
  font-size: 12px;
  letter-spacing: .04em;
}
.consent-brand img {
  width: 28px;
  height: 28px;
  object-fit: contain;
}
.consent-panel {
  position: relative;
  z-index: 1;
  width: 100%;
  max-width: 430px;
  padding: 34px 32px 26px;
  border: 1px solid rgba(255, 255, 255, .82);
  border-radius: 22px;
  background: rgba(255, 255, 255, .9);
  box-shadow: 0 26px 70px rgba(28, 36, 54, .12), 0 2px 8px rgba(28, 36, 54, .04);
  backdrop-filter: blur(10px);
  -webkit-backdrop-filter: blur(10px);
}
.consent-state {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 16px;
  text-align: center;
  color: var(--ink-faint);
  font-size: 13px;
}
.consent-logos {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 13px;
  margin-bottom: 22px;
}
.consent-logo {
  width: 48px;
  height: 48px;
  display: flex;
  align-items: center;
  justify-content: center;
  border: 1px solid rgba(23, 23, 23, .08);
  border-radius: 14px;
  background: #fff;
  box-shadow: 0 6px 18px rgba(28, 36, 54, .07);
  overflow: hidden;
}
.consent-logo img {
  width: 100%;
  height: 100%;
  object-fit: contain;
}
.consent-logo-fallback {
  font-size: 18px;
  font-weight: 600;
  color: var(--ink-soft);
}
.consent-logo-sso { padding: 6px; }
.consent-link {
  width: 34px;
  height: 1px;
  background: linear-gradient(90deg, transparent, var(--line-strong), transparent);
  position: relative;
}
.consent-link::after {
  content: "";
  position: absolute;
  left: 50%;
  top: 50%;
  width: 4px;
  height: 4px;
  margin: -2px 0 0 -2px;
  border-radius: 50%;
  background: var(--brand);
  opacity: .5;
}
.consent-title {
  margin: 0;
  text-align: center;
  font-size: 24px;
  font-weight: 600;
  letter-spacing: -.035em;
  color: var(--ink);
}
.consent-request {
  margin: 12px 0 22px;
  text-align: center;
  font-size: 13px;
  line-height: 1.8;
  color: var(--ink-soft);
}
.consent-account {
  display: flex;
  align-items: center;
  gap: 11px;
  padding: 12px 14px;
  border: 1px solid var(--line-soft);
  border-radius: 14px;
  background: #fbfbfa;
}
.consent-account-avatar {
  width: 36px;
  height: 36px;
  flex: 0 0 auto;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: 50%;
  background: var(--brand-soft);
  font-size: 13px;
  color: var(--brand-ink);
  overflow: hidden;
}
.consent-account-avatar img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.consent-account-main { min-width: 0; flex: 1; }
.consent-account-name {
  font-size: 13px;
  color: var(--ink);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.consent-account-email {
  margin-top: 2px;
  font-size: 11.5px;
  color: var(--ink-faint);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.consent-account-select {
  max-width: 128px;
  padding: 4px 7px;
  border: 1px solid var(--line);
  border-radius: 8px;
  background: #fff;
  color: var(--ink-soft);
  font: inherit;
  font-size: 11px;
}
.consent-error { margin-top: 16px; }
.consent-identities {
  margin-top: 14px;
  padding: 12px 14px;
  border: 1px solid var(--line-soft);
  border-radius: 14px;
  background: #fbfbfa;
}
.consent-identities-label {
  margin-bottom: 8px;
  font-size: 12px;
  font-weight: 600;
  color: var(--ink-soft);
}
.consent-identity {
  display: flex;
  align-items: center;
  gap: 9px;
  padding: 7px 0;
  font-size: 12.5px;
  color: var(--ink);
  cursor: pointer;
}
.consent-identity input { accent-color: var(--brand); }
.consent-identity-main {
  flex: 1;
  min-width: 0;
  display: flex;
  align-items: baseline;
  gap: 8px;
  overflow: hidden;
}
.consent-identity-name { white-space: nowrap; }
.consent-identity-email {
  font-size: 11px;
  color: var(--ink-faint);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.consent-identity-tag {
  flex: 0 0 auto;
  padding: 1px 8px;
  border: 1px solid var(--line);
  border-radius: 999px;
  font-size: 10.5px;
  color: var(--ink-faint);
}
.consent-allow {
  width: 100%;
  height: 46px;
  margin-top: 24px;
  border: 0;
  border-radius: 999px;
  background: var(--brand);
  color: #fff;
  font-size: 14px;
  font-weight: 600;
  box-shadow: 0 10px 24px rgba(51, 112, 255, .22);
}
.consent-allow:hover { background: var(--brand-ink); box-shadow: 0 12px 28px rgba(36, 91, 219, .28); }
.consent-cancel {
  display: block;
  margin: 12px auto 0;
  padding: 4px 10px;
  border: 0;
  background: transparent;
  color: var(--ink-faint);
  font: inherit;
  font-size: 12px;
  cursor: pointer;
}
.consent-cancel:hover { color: var(--ink); }
.consent-note {
  margin: 18px 0 0;
  text-align: center;
  font-size: 11px;
  line-height: 1.7;
  color: var(--ink-faint);
}
@media (max-width: 900px) {
  .consent-decoration { display: none; }
}
@media (max-width: 520px) {
  .consent-page {
    align-items: flex-start;
    padding: 76px 16px 28px;
    background-image:
      radial-gradient(500px 320px at 85% 10%, rgba(51, 112, 255, .06), transparent 70%),
      url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='48' height='48'%3E%3Cpath d='M24 20v8M20 24h8' stroke='%23d3d1ca' stroke-width='1'/%3E%3Cpath d='M0 24h48M24 0v48' stroke='%23eeede8' stroke-width='1'/%3E%3C/svg%3E");
    background-size: auto, 48px 48px;
  }
  .consent-brand {
    top: 24px;
    left: 20px;
  }
  .consent-panel {
    max-width: 100%;
    padding: 28px 20px 22px;
    border-radius: 18px;
    background: rgba(255, 255, 255, .96);
  }
  .consent-logo { width: 44px; height: 44px; }
  .consent-title { font-size: 21px; }
}
</style>
