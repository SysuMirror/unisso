<template>
  <AuthShell>
    <form v-if="!qrMode && loginMode === 'password'" class="auth-form" @submit.prevent="submit">
      <div v-if="error" class="auth-error">{{ error }}</div>

      <label class="auth-field">
        <span>邮箱</span>
        <input id="login-email" v-model.trim="email" type="email" required autocomplete="email"
               placeholder="you@example.com" autofocus />
      </label>

      <label class="auth-field">
        <span>密码</span>
        <input id="login-password" v-model="password" type="password" required autocomplete="current-password"
               placeholder="••••••••" />
      </label>

      <button class="auth-submit" type="submit" :disabled="loading">
        {{ loading ? '登录中…' : '登录' }}
      </button>
      <div class="login-links">
        <button class="auth-alt-link" type="button" @click="switchMode('emailCode')">
          邮箱验证码登录
        </button>
        <RouterLink class="auth-alt-link" to="/reset-password">忘记密码？</RouterLink>
      </div>
    </form>

    <form v-else-if="!qrMode" class="auth-form" @submit.prevent="submitEmailCode">
      <div v-if="error" class="auth-error">{{ error }}</div>
      <div v-if="notice" class="auth-notice">{{ notice }}</div>

      <label class="auth-field">
        <span>邮箱</span>
        <input v-model.trim="email" type="email" required autocomplete="email"
               placeholder="you@example.com" autofocus />
      </label>

      <button class="auth-submit auth-submit-secondary" type="button"
              :disabled="codeSending || codeCooldown > 0 || !email"
              @click="sendEmailCode">
        {{ codeSending ? '发送中…' : codeCooldown > 0 ? `${codeCooldown}s 后可重发` : '发送验证码' }}
      </button>

      <label class="auth-field">
        <span>验证码</span>
        <input v-model.trim="code" inputmode="numeric" maxlength="6" required
               placeholder="6 位验证码" autocomplete="one-time-code" />
      </label>

      <button class="auth-submit" type="submit" :disabled="loading">
        {{ loading ? '登录中…' : '验证码登录' }}
      </button>
      <div class="login-links">
        <button class="auth-alt-link" type="button" @click="switchMode('password')">
          返回密码登录
        </button>
      </div>
    </form>

    <div v-else class="auth-qr">
      <div class="qr-seg">
        <button type="button" class="qr-seg-btn" :class="{ 'is-active': qrProvider === 'sso' }"
                :disabled="qrLoading" @click="switchQr('sso')">手机扫码</button>
        <button v-if="qrEnabled" type="button" class="qr-seg-btn"
                :class="{ 'is-active': qrProvider === 'sse' }"
                :disabled="qrLoading" @click="switchQr('sse')">集市扫码</button>
      </div>
      <div v-if="qrError" class="auth-error">{{ qrError }}</div>
      <div v-else-if="qrExpired" class="auth-qr-expired">
        <p>二维码已过期</p>
        <button class="auth-alt-link" type="button" @click="startQr(qrProvider)">刷新二维码</button>
      </div>
      <div v-else-if="qrDenied" class="auth-qr-expired">
        <p>手机上已拒绝本次登录</p>
        <button class="auth-alt-link" type="button" @click="startQr(qrProvider)">重新生成二维码</button>
      </div>
      <div v-else-if="qrScanned" class="qr-scanned">
        <span class="qr-check" aria-hidden="true">
          <svg viewBox="0 0 24 24" width="30" height="30" fill="none" stroke="currentColor"
               stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M20 6 9 17l-5-5" />
          </svg>
        </span>
        <p class="qr-scanned-text">已扫码，请在手机上确认登录</p>
        <p v-if="qrRemain > 0" class="qr-sub-hint">确认成功后本页会自动跳转</p>
      </div>
      <template v-else>
        <img v-if="qrDataUrl" :src="qrDataUrl" class="auth-qr-img" alt="扫码登录二维码" />
        <p class="auth-qr-hint">{{ qrHint }}</p>
        <p class="qr-sub-hint">{{ qrSubHint }}</p>
        <p v-if="qrRemain > 0" class="qr-count" :class="{ 'is-low': qrRemain <= 60 }">
          {{ qrCountdown }} 后二维码失效
        </p>
      </template>
      <button class="auth-alt-link" type="button" @click="backToPassword">返回密码登录</button>
    </div>

    <div v-if="!qrMode" class="auth-alt qr-alt-row">
      <button class="auth-alt-link" type="button" :disabled="qrLoading" @click="startQr('sso')">
        {{ qrLoading && qrProvider === 'sso' ? '正在生成二维码…' : '手机扫码登录' }}
      </button>
      <button v-if="qrEnabled" class="auth-alt-link" type="button"
              :disabled="qrLoading" @click="startQr('sse')">
        {{ qrLoading && qrProvider === 'sse' ? '正在生成二维码…' : '集市账号扫码登录' }}
      </button>
    </div>

    <template #note>
      没有账号？<RouterLink to="/register">注册</RouterLink>
    </template>
  </AuthShell>
</template>

<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import QRCode from 'qrcode'
import AuthShell from '../components/AuthShell.vue'
import { api, resetCsrf } from '../api'
import { loadMe } from '../store'

const route = useRoute()
const router = useRouter()
const email = ref('')
const password = ref('')
const code = ref('')
const loginMode = ref('password')
const loading = ref(false)
const codeSending = ref(false)
const codeCooldown = ref(0)
const error = ref('')
const notice = ref('')
let codeTimer = null

const qrEnabled = ref(false)
const qrMode = ref(false)
const qrProvider = ref('sso')
const qrLoading = ref(false)
const qrDataUrl = ref('')
const qrState = ref('')
const qrId = ref('')
const qrError = ref('')
const qrExpired = ref(false)
const qrDenied = ref(false)
const qrScanned = ref(false)
const qrRemain = ref(0)
let pollTimer = null
let countdownTimer = null

const qrCountdown = computed(() => {
  const s = Math.max(0, qrRemain.value)
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`
})

function stopCountdown() {
  if (countdownTimer) {
    clearInterval(countdownTimer)
    countdownTimer = null
  }
  qrRemain.value = 0
}

function startCountdown(seconds) {
  stopCountdown()
  qrRemain.value = seconds
  countdownTimer = setInterval(() => {
    qrRemain.value -= 1
    if (qrRemain.value <= 0) {
      stopPolling()
      stopCountdown()
      qrExpired.value = true
    }
  }, 1000)
}

const qrHint = computed(() => qrProvider.value === 'sse'
  ? '用已登录集市的手机扫码，按页面提示完成授权'
  : '用手机浏览器扫码，并在手机上确认即可登录')
const qrSubHint = computed(() => qrProvider.value === 'sse'
  ? '适用于：手机已登录集市账号'
  : '适用于：手机已登录统一身份认证；未登录也没关系，扫码后先在手机上登录即可')

function safeNext(value) {
  return typeof value === 'string' && value.startsWith('/') && !value.startsWith('//') ? value : '/'
}

function switchMode(mode) {
  error.value = ''
  notice.value = ''
  loginMode.value = mode
}

function startCodeCooldown(seconds) {
  codeCooldown.value = seconds
  if (codeTimer) clearInterval(codeTimer)
  codeTimer = setInterval(() => {
    codeCooldown.value -= 1
    if (codeCooldown.value <= 0) {
      clearInterval(codeTimer)
      codeTimer = null
    }
  }, 1000)
}

async function sendEmailCode() {
  if (!email.value || codeSending.value || codeCooldown.value > 0) return
  error.value = ''
  notice.value = ''
  codeSending.value = true
  try {
    const data = await api('/api/auth/email-code/start', {
      method: 'POST',
      body: { email: email.value }
    })
    notice.value = '验证码已发送，请查收邮箱。'
    startCodeCooldown(data?.cooldown || 60)
  } catch (e) {
    error.value = e.message
  } finally {
    codeSending.value = false
  }
}

async function submitEmailCode() {
  error.value = ''
  notice.value = ''
  loading.value = true
  try {
    const next = safeNext(route.query.next)
    const data = await api('/api/auth/email-code/verify', {
      method: 'POST',
      body: { email: email.value, code: code.value, next }
    })
    resetCsrf()
    await loadMe(true)
    router.push(safeNext(data?.next || next))
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

async function submit() {
  error.value = ''
  loading.value = true
  try {
    const next = safeNext(route.query.next)
    const data = await api('/api/auth/login', {
      method: 'POST',
      body: { email: email.value, password: password.value, next }
    })
    if (data?.step_up_required) {
      sessionStorage.setItem('unisso.stepup.token', data.step_up_token || '')
      sessionStorage.setItem('unisso.stepup.email', data.email_hint || '')
      sessionStorage.setItem('unisso.stepup.next', safeNext(data.next || next))
      sessionStorage.setItem('unisso.stepup.methods', JSON.stringify(data.methods || ['email']))
      sessionStorage.setItem('unisso.stepup.trustDays', String(data.trust_days || 180))
      resetCsrf()
      router.push('/step-up')
      return
    }
    resetCsrf()
    await loadMe(true)
    router.push(safeNext(data?.next || next))
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = null
  }
}

async function poll() {
  try {
    if (qrProvider.value === 'sse') {
      if (!qrState.value) return
      const data = await api('/api/auth/sse/qr/poll?state=' + encodeURIComponent(qrState.value))
      if (data.status === 'authorized') {
        stopPolling()
        resetCsrf()
        await loadMe(true)
        router.push('/')
      } else if (data.status === 'expired') {
        stopPolling()
        stopCountdown()
        qrExpired.value = true
      }
      return
    }
    if (!qrId.value) return
    const data = await api('/api/auth/qr/poll?id=' + encodeURIComponent(qrId.value))
    if (data.status === 'ok') {
      stopPolling()
      resetCsrf()
      await loadMe(true)
      router.push(safeNext(data?.next))
    } else if (data.status === 'scanned') {
      qrScanned.value = true
    } else if (data.status === 'expired') {
      stopPolling()
      stopCountdown()
      qrExpired.value = true
    } else if (data.status === 'denied') {
      stopPolling()
      stopCountdown()
      qrDenied.value = true
    }
  } catch {
    // Transient polling errors are ignored.
  }
}

async function startQr(provider = 'sso') {
  qrError.value = ''
  qrExpired.value = false
  qrDenied.value = false
  qrScanned.value = false
  qrLoading.value = true
  stopPolling()
  stopCountdown()
  try {
    let url = ''
    let expires = 300
    if (provider === 'sse') {
      const data = await api('/api/auth/sse/qr/start')
      qrState.value = data.state
      qrId.value = ''
      url = data.qr_url
      expires = data.expires_in > 0 ? data.expires_in : 300
    } else {
      const next = safeNext(route.query.next)
      const data = await api('/api/auth/qr/start?next=' + encodeURIComponent(next))
      qrId.value = data.id
      qrState.value = ''
      url = data.qr_url
      expires = data.expires_in > 0 ? data.expires_in : 300
    }
    qrDataUrl.value = await QRCode.toDataURL(url, {
      margin: 2,
      width: 220,
      color: { dark: '#171717', light: '#fbfbfa' }
    })
    qrProvider.value = provider
    qrMode.value = true
    pollTimer = setInterval(poll, 1200)
    startCountdown(expires)
  } catch (e) {
    qrProvider.value = provider
    qrMode.value = true
    qrError.value = e.message
  } finally {
    qrLoading.value = false
  }
}

function switchQr(provider) {
  if (provider === qrProvider.value && qrDataUrl.value && !qrExpired.value) return
  startQr(provider)
}

function backToPassword() {
  stopPolling()
  stopCountdown()
  qrMode.value = false
  qrError.value = ''
  qrExpired.value = false
  qrDenied.value = false
  qrScanned.value = false
  loginMode.value = 'password'
}

onMounted(async () => {
  // Phone opened a /qr/<id> link but is not logged in yet: the router sent it
  // here with next=/qr/<id>. Tell the PC “已扫码” right away.
  const nextPath = typeof route.query.next === 'string' ? route.query.next : ''
  const qrOpen = nextPath.match(/^\/qr\/([\w-]{16,64})$/)
  if (qrOpen) api('/api/auth/qr/scan?id=' + encodeURIComponent(qrOpen[1])).catch(() => {})
  try {
    const data = await api('/api/auth/sse/qr/status')
    qrEnabled.value = !!data.enabled
  } catch {
    qrEnabled.value = false
  }
})

onUnmounted(() => {
  stopPolling()
  stopCountdown()
  if (codeTimer) clearInterval(codeTimer)
})
</script>

<style scoped>
.login-links {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-top: 14px;
}
.qr-alt-row {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 18px;
  flex-wrap: wrap;
}
.qr-seg {
  display: flex;
  gap: 6px;
  justify-content: center;
  margin-bottom: 18px;
}
.qr-seg-btn {
  border: 1px solid var(--line, #e6e6e2);
  background: transparent;
  color: var(--ink-soft, #666);
  font-size: 13px;
  padding: 6px 16px;
  border-radius: 999px;
  cursor: pointer;
}
.qr-seg-btn.is-active {
  border-color: var(--brand, #171717);
  background: var(--brand, #171717);
  color: #fff;
}
.qr-seg-btn:disabled { opacity: .5; cursor: not-allowed; }
.qr-sub-hint {
  margin: 6px 0 0;
  font-size: 12px;
  color: var(--ink-soft, #8a8a86);
  line-height: 1.6;
}
.qr-count {
  margin: 10px 0 0;
  font-size: 12px;
  color: var(--ink-soft, #8a8a86);
  font-variant-numeric: tabular-nums;
}
.qr-count.is-low { color: #c2410c; }
.qr-scanned {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  padding: 22px 0 6px;
}
.qr-check {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 58px;
  height: 58px;
  border-radius: 50%;
  color: #16a34a;
  background: rgba(22, 163, 74, .1);
}
.qr-scanned-text { margin: 8px 0 0; font-size: 15px; font-weight: 600; }
</style>
