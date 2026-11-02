<template>
  <AuthShell kicker="二次验证">
    <form class="auth-form" @submit.prevent="verify">
      <div v-if="error" class="auth-error">{{ error }}</div>
      <div v-if="notice" class="auth-notice">{{ notice }}</div>

      <p class="stepup-copy">
        当前设备是新设备，或账号较长时间未登录。为保护账号，请再完成一次验证。
      </p>

      <template v-if="hasEmail || hasFace || hasDevice">
        <label class="stepup-trust">
          <input v-model="trustDevice" type="checkbox" />
          <span>在这台设备上信任 {{ trustDays }} 天</span>
        </label>
        <p v-if="trustDevice" class="stepup-trust-risk">
          仅在你自己的设备上开启。开启后，这台设备登录时不再要求二次验证；
          能使用这台设备的人可以直接进入你的账号。账号连续多天未登录时仍会要求验证。
          可随时在“个人中心 → 登录设备”移除信任。
        </p>
        <p v-else class="stepup-trust-hint">
          只建议在自己的设备上开启长期信任；账号长时间未登录时仍会要求验证。
        </p>
      </template>

      <div v-if="hasDevice" class="stepup-device">
        <div class="stepup-device-head">
          <span class="stepup-device-dot"></span>
          已发送到已登录设备
        </div>
        <p v-if="deviceStatus === 'starting' || deviceStatus === 'pending'" class="stepup-device-copy">
          请在已登录的设备上点“允许登录”。这里会自动继续，不需要刷新。
        </p>
        <p v-else-if="deviceStatus === 'denied'" class="stepup-device-copy">
          已在已登录设备上拒绝本次登录。你可以重新发送，或改用其他方式。
        </p>
        <p v-else-if="deviceStatus === 'expired'" class="stepup-device-copy">
          确认请求已过期，请重新发送或改用其他方式。
        </p>
        <p v-else-if="deviceStatus === 'canceled'" class="stepup-device-copy">
          该确认请求已取消，请重新发送或改用其他方式。
        </p>
        <p v-else-if="deviceStatus === 'error'" class="stepup-device-copy stepup-device-error">
          {{ deviceError }}
        </p>
        <div class="stepup-device-actions">
          <span v-if="deviceStatus === 'starting' || deviceStatus === 'pending'"
                class="stepup-device-wait">等待确认…</span>
          <button v-else class="btn btn-secondary btn-sm" type="button"
                  :disabled="deviceStarting" @click="startDeviceApproval(true)">
            {{ deviceStarting ? '发送中…' : '重新发送' }}
          </button>
        </div>
      </div>

      <p v-if="hasDevice && (hasEmail || hasFace)" class="stepup-or">或使用其他方式</p>

      <template v-if="hasEmail">
        <p v-if="emailHint" class="stepup-target">
          验证码将发送至 <strong>{{ emailHint }}</strong>
        </p>
        <button class="auth-submit auth-submit-secondary" type="button"
                :disabled="sending || cooldown > 0" @click="sendCode">
          {{ sending ? '发送中…' : cooldown > 0 ? `${cooldown}s 后可重发` : '发送邮箱验证码' }}
        </button>

        <label class="auth-field">
          <span>验证码</span>
          <input v-model.trim="code" inputmode="numeric" maxlength="6" required
                 placeholder="6 位验证码" autocomplete="one-time-code" autofocus />
        </label>

        <button class="auth-submit" type="submit" :disabled="verifying">
          {{ verifying ? '验证中…' : '验证并登录' }}
        </button>
      </template>

      <template v-if="hasFace">
        <div v-if="hasEmail" class="stepup-divider"><span>或</span></div>
        <button class="auth-submit" :class="{ 'auth-submit-secondary': hasEmail }"
                type="button" :disabled="faceUploading" @click="startFace">
          {{ faceUploading ? '准备中…' : '使用人脸验证' }}
        </button>
      </template>

      <p v-if="!hasEmail && !hasFace" class="stepup-target">
        当前账号没有可用的验证方式，请返回登录页重试。
      </p>

      <button class="auth-alt-link" type="button" @click="cancel">取消验证，返回登录</button>
    </form>

    <Modal :open="faceMode" wide @close="closeFace">
      <template #title>人脸验证</template>
      <div v-if="faceError" class="face-stepup-error">
        <p class="face-stepup-error-title">{{ faceError }}</p>
        <p class="face-stepup-error-tip">请面向光源，保持整张脸在框内，然后重试。</p>
        <div class="face-stepup-actions">
          <button class="btn btn-primary" type="button"
                  :disabled="faceUploading" @click="startFace">重新验证</button>
          <button v-if="hasEmail" class="btn btn-secondary" type="button"
                  @click="closeFace">改用邮箱验证码</button>
          <button class="btn btn-ghost" type="button" @click="closeFace">取消</button>
        </div>
      </div>
      <div v-else-if="faceSession" class="face-stepup">
        <p class="face-stepup-lead">按提示完成动作，验证通过后会直接登录。</p>
        <FaceCapture :session="faceSession" purpose="verify" :busy="faceUploading"
                     @captured="verifyFace" @cancel="closeFace" />
      </div>
      <div v-else class="face-stepup-loading">正在准备摄像头…</div>
    </Modal>
  </AuthShell>
</template>

<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AuthShell from '../components/AuthShell.vue'
import Modal from '../components/Modal.vue'
import FaceCapture from '../components/FaceCapture.vue'
import { api, resetCsrf } from '../api'
import { loadMe } from '../store'

const router = useRouter()
const route = useRoute()
const token = ref('')
const emailHint = ref('')
const next = ref('/')
const methods = ref(['email'])
const trustDevice = ref(false)
const trustDays = ref(180)
const hasEmail = computed(() => methods.value.includes('email'))
const hasFace = computed(() => methods.value.includes('face'))
const hasDevice = computed(() => methods.value.includes('device'))
const faceMode = ref(false)
const deviceStatus = ref('idle')
const deviceError = ref('')
const deviceStarted = ref(false)
const deviceStarting = ref(false)
let deviceTimer = null
let devicePolling = false
const faceSession = ref(null)
const faceUploading = ref(false)
const faceError = ref('')
const code = ref('')
const error = ref('')
const notice = ref('')
const sending = ref(false)
const verifying = ref(false)
const cooldown = ref(0)
let timer = null

function clearState() {
  sessionStorage.removeItem('unisso.stepup.token')
  sessionStorage.removeItem('unisso.stepup.email')
  sessionStorage.removeItem('unisso.stepup.next')
  sessionStorage.removeItem('unisso.stepup.methods')
  sessionStorage.removeItem('unisso.stepup.trustDays')
}

function persistMethods() {
  sessionStorage.setItem('unisso.stepup.methods', JSON.stringify(methods.value))
}

function closeFace() {
  faceMode.value = false
  faceSession.value = null
  faceError.value = ''
}

async function refreshMethods() {
  if (!token.value) return
  try {
    const data = await api('/api/auth/step-up/methods', {
      method: 'POST',
      body: { token: token.value }
    })
    if (Array.isArray(data?.methods)) {
      methods.value = data.methods
      persistMethods()
    }
    if (data?.email_hint) emailHint.value = data.email_hint
    if (data?.trust_days) trustDays.value = Number(data.trust_days) || 180
    if (!hasFace.value) closeFace()
    if (hasDevice.value) startDeviceApproval()
  } catch (e) {
    if (e.status === 401 || e.status === 403) {
      clearState()
      router.replace('/login')
    }
  }
}

function stopDevicePoll() {
  if (deviceTimer) {
    clearInterval(deviceTimer)
    deviceTimer = null
  }
}

async function pollDeviceStatus() {
  if (devicePolling || !token.value) return
  devicePolling = true
  try {
    const data = await api('/api/auth/step-up/device/status', {
      method: 'POST',
      body: { token: token.value, trust_device: trustDevice.value }
    })
    const status = data?.status || 'pending'
    if (status === 'approved' || status === 'consumed') {
      stopDevicePoll()
      deviceStatus.value = 'approved'
      const target = data?.next || next.value || '/'
      clearState()
      resetCsrf()
      await loadMe(true)
      router.replace(target.startsWith('/') && !target.startsWith('//') ? target : '/')
      return
    }
    if (status === 'denied' || status === 'expired' || status === 'canceled') {
      stopDevicePoll()
      deviceStatus.value = status
      return
    }
    if (status === 'none') {
      stopDevicePoll()
      deviceStatus.value = 'error'
      deviceError.value = '设备确认请求不存在，请重新发送。'
      return
    }
    deviceStatus.value = 'pending'
  } catch (e) {
    if (e.status === 401 || e.status === 403 ||
        e.data?.error === 'step_up_expired' || e.data?.error === 'step_up_required') {
      stopDevicePoll()
      clearState()
      router.replace('/login')
      return
    }
    deviceStatus.value = 'error'
    deviceError.value = e.message
  } finally {
    devicePolling = false
  }
}

async function startDeviceApproval(force = false) {
  if (!token.value || deviceStarting.value) return
  if (deviceStarted.value && !force) return
  deviceStarted.value = true
  deviceStarting.value = true
  deviceError.value = ''
  deviceStatus.value = 'starting'
  stopDevicePoll()
  try {
    await api('/api/auth/step-up/device/start', {
      method: 'POST',
      body: { token: token.value, trust_device: trustDevice.value }
    })
    deviceStatus.value = 'pending'
    deviceTimer = setInterval(pollDeviceStatus, 2000)
    pollDeviceStatus()
  } catch (e) {
    deviceStatus.value = 'error'
    deviceError.value = e.message
    if (e.status === 401 || e.status === 403) {
      clearState()
      router.replace('/login')
    }
  } finally {
    deviceStarting.value = false
  }
}

function startCooldown(seconds) {
  cooldown.value = seconds
  if (timer) clearInterval(timer)
  timer = setInterval(() => {
    cooldown.value -= 1
    if (cooldown.value <= 0) {
      clearInterval(timer)
      timer = null
    }
  }, 1000)
}

async function startFace() {
  if (!token.value || faceUploading.value) return
  error.value = ''
  notice.value = ''
  faceError.value = ''
  faceSession.value = null
  faceMode.value = true
  faceUploading.value = true
  try {
    const data = await api('/api/auth/step-up/face/session', {
      method: 'POST',
      body: { token: token.value }
    })
    faceSession.value = data
  } catch (e) {
    if (e.data?.error === 'face_not_available') {
      methods.value = methods.value.filter((method) => method !== 'face')
      persistMethods()
      closeFace()
      notice.value = hasEmail.value
        ? '当前账号没有可用人脸，请使用邮箱验证码。'
        : '当前账号没有可用的人脸验证方式。'
      return
    }
    if (e.status === 401 || e.status === 403) {
      clearState()
      router.replace('/login')
      return
    }
    faceError.value = e.message
  } finally {
    faceUploading.value = false
  }
}

async function verifyFace(frames) {
  if (!frames?.length) {
    faceError.value = '没有采集到足够的清晰画面：请放慢动作、面向光源，保持整张脸在框内后重试。'
    faceSession.value = null
    faceMode.value = true
    return
  }
  if (!faceSession.value) return
  faceUploading.value = true
  error.value = ''
  notice.value = ''
  try {
    const form = new FormData()
    form.append('token', token.value)
    form.append('session_token', faceSession.value.session_token)
    form.append('trust_device', trustDevice.value ? 'true' : 'false')
    frames.forEach((frame, index) => form.append('frames', frame, `frame-${index}.jpg`))
    const data = await api('/api/auth/step-up/face/verify', { method: 'POST', body: form })
    const target = data?.next || next.value || '/'
    faceError.value = ''
    clearState()
    resetCsrf()
    await loadMe(true)
    router.replace(target.startsWith('/') && !target.startsWith('//') ? target : '/')
  } catch (e) {
    if (e.data?.error === 'step_up_expired' || e.data?.error === 'step_up_required' ||
        e.data?.error === 'invalid_account') {
      clearState()
      router.replace('/login')
      return
    }
    faceError.value = e.message
    faceSession.value = null
    faceMode.value = true
  } finally {
    faceUploading.value = false
  }
}

async function sendCode() {
  if (!token.value || sending.value || cooldown.value > 0) return
  error.value = ''
  notice.value = ''
  sending.value = true
  try {
    const data = await api('/api/auth/step-up/email', {
      method: 'POST',
      body: { token: token.value }
    })
    emailHint.value = data?.email_hint || emailHint.value
    notice.value = '验证码已发送，请查收邮箱。'
    startCooldown(60)
  } catch (e) {
    const errCode = e.data?.error
    if (errCode === 'step_up_expired' || errCode === 'step_up_required') {
      clearState()
      router.replace('/login')
      return
    }
    error.value = e.message
  } finally {
    sending.value = false
  }
}

async function verify() {
  if (!token.value || !hasEmail.value) return
  error.value = ''
  notice.value = ''
  verifying.value = true
  try {
    const data = await api('/api/auth/step-up/verify', {
      method: 'POST',
      body: { token: token.value, code: code.value, trust_device: trustDevice.value }
    })
    const target = data?.next || next.value || '/'
    faceError.value = ''
    clearState()
    resetCsrf()
    await loadMe(true)
    router.replace(target.startsWith('/') && !target.startsWith('//') ? target : '/')
  } catch (e) {
    // Only leave the page when the challenge itself is dead. A wrong code is
    // a plain 401 invalid_code — the challenge is still alive, keep it so the
    // user can just retype the code instead of logging in again.
    const errCode = e.data?.error
    if (errCode === 'step_up_expired' || errCode === 'step_up_required' ||
        errCode === 'invalid_account' || errCode === 'too_many_attempts') {
      clearState()
      router.replace('/login')
      return
    }
    error.value = e.message
  } finally {
    verifying.value = false
  }
}

async function cancel() {
  try {
    if (token.value) {
      await api('/api/auth/step-up/cancel', {
        method: 'POST',
        body: { token: token.value }
      })
    }
  } catch {
    // Ignore; local state is cleared regardless.
  }
  clearState()
  resetCsrf()
  router.replace('/login')
}

onMounted(() => {
  // Direct navigations (e.g. the marketplace same-device callback) carry the
  // token in the URL; XHR-driven flows keep it in sessionStorage.
  const queryToken = typeof route.query.token === 'string' ? route.query.token : ''
  if (queryToken) {
    sessionStorage.setItem('unisso.stepup.token', queryToken)
    const queryNext = typeof route.query.next === 'string' && route.query.next.startsWith('/')
      && !route.query.next.startsWith('//') ? route.query.next : ''
    if (queryNext) sessionStorage.setItem('unisso.stepup.next', queryNext)
  }
  token.value = sessionStorage.getItem('unisso.stepup.token') || ''
  emailHint.value = sessionStorage.getItem('unisso.stepup.email') || ''
  next.value = sessionStorage.getItem('unisso.stepup.next') || '/'
  trustDays.value = Number(sessionStorage.getItem('unisso.stepup.trustDays')) || 180
  try {
    const stored = JSON.parse(sessionStorage.getItem('unisso.stepup.methods') || '["email"]')
    if (Array.isArray(stored) && stored.length) methods.value = stored
  } catch {
    methods.value = ['email']
  }
  if (!token.value) {
    router.replace('/login')
    return
  }
  if (hasDevice.value) startDeviceApproval()
  refreshMethods()
})

onUnmounted(() => {
  if (timer) clearInterval(timer)
  stopDevicePoll()
})
</script>

<style scoped>
.auth-notice {
  margin-bottom: 16px;
  padding: 10px 12px;
  border-left: 2px solid var(--ok);
  background: var(--ok-soft);
  color: var(--ok);
  font-size: 12.5px;
  line-height: 1.6;
}
.auth-submit-secondary {
  background: transparent;
  border: 1px solid var(--line-strong);
  color: var(--ink);
}
.auth-submit-secondary:hover {
  background: var(--fill);
}
.stepup-copy {
  margin: 0 0 8px;
  font-size: 13px;
  line-height: 1.7;
  color: var(--ink-soft);
}
.stepup-target {
  margin: 0 0 18px;
  font-size: 12.5px;
  color: var(--ink-faint);
}
.stepup-target strong {
  color: var(--ink);
  font-weight: 500;
}
.stepup-trust {
  display: flex;
  align-items: flex-start;
  gap: 9px;
  margin-top: 16px;
  font-size: 12.5px;
  line-height: 1.6;
  color: var(--ink-soft);
  cursor: pointer;
}
.stepup-trust input {
  width: 15px;
  height: 15px;
  margin: 2px 0 0;
  flex: 0 0 auto;
  accent-color: var(--ink);
}
.stepup-trust-risk {
  margin: 8px 0 0;
  padding: 9px 11px;
  border-left: 2px solid var(--warn);
  background: var(--warn-soft);
  font-size: 11.5px;
  line-height: 1.7;
  color: var(--warn);
}
.stepup-trust-hint {
  margin: 7px 0 0;
  font-size: 11px;
  color: var(--ink-faint);
}
.stepup-device {
  margin-top: 16px;
  padding: 13px 14px;
  border: 1px solid var(--line);
  border-radius: 12px;
  background: var(--fill);
}
.stepup-device-head {
  display: flex;
  align-items: center;
  gap: 7px;
  font-size: 12.5px;
  color: var(--ink);
}
.stepup-device-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: var(--brand);
  box-shadow: 0 0 0 4px var(--brand-soft);
}
.stepup-device-copy {
  margin: 8px 0 0;
  font-size: 11.5px;
  line-height: 1.7;
  color: var(--ink-soft);
}
.stepup-device-error { color: var(--danger); }
.stepup-device-actions {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-top: 10px;
}
.stepup-device-wait { font-size: 11.5px; color: var(--ink-faint); }
.stepup-or {
  margin: 16px 0 10px;
  text-align: center;
  font-size: 11px;
  color: var(--ink-faint);
}
.stepup-divider {
  display: flex;
  align-items: center;
  gap: 12px;
  margin: 18px 0;
  font-size: 11px;
  color: var(--ink-faint);
}
.stepup-divider::before,
.stepup-divider::after {
  content: "";
  flex: 1;
  height: 1px;
  background: var(--line);
}
.face-stepup {
  margin-top: 4px;
}
.face-stepup-lead {
  margin: 0 0 14px;
  font-size: 12.5px;
  line-height: 1.7;
  color: var(--ink-soft);
}
.face-stepup-loading {
  padding: 32px 0;
  text-align: center;
  font-size: 13px;
  color: var(--ink-faint);
}
.face-stepup-error {
  padding: 4px 0;
}
.face-stepup-error-title {
  margin: 0 0 8px;
  color: var(--danger);
  font-size: 12.5px;
  line-height: 1.7;
}
.face-stepup-error-tip {
  margin: 0 0 12px;
  color: var(--ink-faint);
  font-size: 12px;
  line-height: 1.7;
}
.face-stepup-actions {
  display: grid;
  gap: 8px;
}
.face-stepup-actions .auth-submit-secondary {
  margin-bottom: 0;
}
.face-stepup .auth-alt-link {
  margin-top: 8px;
}
.stepup-hint {
  margin: 18px 0 0;
  padding-top: 14px;
  border-top: 1px solid var(--line);
  font-size: 12px;
  line-height: 1.7;
  color: var(--ink-faint);
}
.auth-submit-secondary {
  margin-bottom: 12px;
}
</style>
