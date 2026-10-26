<template>
  <section class="section">
    <div class="section-head">
      <div>
        <h2 class="section-title">人脸二次验证</h2>
        <p class="section-sub">仅用于登录时的二次验证，不能代替密码；不会保存照片。</p>
      </div>
      <button
        v-if="enabled && mode === 'idle' && faces.length"
        class="btn btn-secondary btn-sm"
        type="button"
        :disabled="!consent"
        @click="startProbe"
      >
        再登记一张
      </button>
    </div>

    <div v-if="error" class="alert alert-danger">{{ error }}</div>
    <div v-if="notice" class="alert alert-success">{{ notice }}</div>

    <div v-if="!enabled" class="empty-state">
      <h3>人脸服务尚未启用</h3>
      <p>当前可以使用邮箱验证码完成二次验证。</p>
    </div>

    <template v-else>
      <div v-if="mode === 'question'" class="face-flow">
        <h3 class="face-flow-title">这张人脸已登记</h3>
        <p class="face-flow-lead">
          这是你本人吗？是本人请先验证邮箱；不是本人请重新采集。
        </p>
        <div class="face-flow-actions">
          <button class="btn btn-primary" type="button" :disabled="busy" @click="sendBindCode">
            {{ busy ? '发送中…' : '是，这是我的脸' }}
          </button>
          <button class="btn btn-secondary" type="button" :disabled="busy" @click="startEnroll">
            不是，重新采集
          </button>
        </div>
      </div>

      <div v-else-if="mode === 'email'" class="face-flow">
        <h3 class="face-flow-title">验证邮箱</h3>
        <p class="face-flow-lead">验证码已发送至 {{ emailHint }}，输入后即可绑定这张人脸。</p>
        <form class="face-code-form" @submit.prevent="confirmBind">
          <input
            v-model.trim="code"
            class="form-input"
            inputmode="numeric"
            maxlength="6"
            required
            placeholder="6 位验证码"
            autocomplete="one-time-code"
          />
          <button class="btn btn-primary" type="submit" :disabled="busy">
            {{ busy ? '验证中…' : '确认绑定' }}
          </button>
        </form>
        <button class="text-link" type="button" @click="startEnroll">不是我，重新采集</button>
      </div>

      <template v-else>
        <div v-if="faces.length" class="table-wrap face-list">
          <table class="table">
            <thead>
              <tr><th>人脸</th><th>登记时间</th><th>操作</th></tr>
            </thead>
            <tbody>
              <tr v-for="(face, index) in faces" :key="face.id">
                <td class="cell-main">人脸 {{ index + 1 }}</td>
                <td class="text-secondary">{{ formatDate(face.created_at) }}</td>
                <td>
                  <button class="btn btn-danger btn-sm" type="button" :disabled="busy"
                          @click="removeFace(face)">解除</button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>

        <div v-else class="face-recommend">
          <h3>建议登记人脸</h3>
          <p>新设备或长期未登录时可选人脸验证，降低账号丢失风险。</p>
          <p class="face-recommend-meta">不会保存照片，可随时解除。</p>
        </div>

        <label class="consent-row">
          <input v-model="consent" type="checkbox" :disabled="!agreementRead" />
          <span>
            我已阅读并同意
            <button class="agreement-link" type="button" @click.prevent.stop="openAgreement">《人脸识别服务协议》</button>
          </span>
        </label>
        <p v-if="!agreementRead" class="face-consent-hint">
          打开协议并向下滑动，5 秒后可勾选。
        </p>
        <p v-else class="face-consent-hint">已阅读并同意，可开始登记。</p>
        <button v-if="!faces.length" class="btn btn-primary" type="button"
                :disabled="!consent" @click="startProbe">开始登记</button>
      </template>
    </template>
  </section>

  <Modal :open="mode === 'capture'" wide @close="resetFlow">
    <template #title>{{ capturePurpose === 'probe' ? '检测人脸' : '登记人脸' }}</template>
    <template v-if="!captureError">
      <p class="capture-lead">请正对镜头，按提示完成动作。</p>
      <FaceCapture
        v-if="mode === 'capture'"
        :session="session"
        :purpose="capturePurpose"
        :busy="uploading"
        @captured="onCaptured"
        @cancel="resetFlow"
      />
    </template>
    <div v-else class="capture-error">
      <p class="capture-error-title">{{ captureError }}</p>
      <p class="capture-error-tip">
        请面向光源，保持整张脸在框内，然后重试。
      </p>
      <div class="face-flow-actions">
        <button class="btn btn-primary" type="button" :disabled="busy" @click="retryCapture">
          {{ busy ? '准备中…' : '重新采集' }}
        </button>
        <button class="btn btn-secondary" type="button" :disabled="busy" @click="resetFlow">
          取消
        </button>
      </div>
    </div>
  </Modal>

  <Modal :open="agreementOpen" @close="closeAgreement">
    <template #title>人脸识别服务协议</template>
    <div class="agreement-status">
      <span v-if="agreementRead">已阅读并同意</span>
      <span v-else>
        向下滑动阅读，{{ agreementSeconds }}/5 秒后可勾选
      </span>
    </div>
    <div ref="agreementScroll" class="agreement-scroll" @scroll="onAgreementScroll">
      <FaceAgreement />
    </div>
    <div class="modal-actions">
      <button class="btn btn-ghost" type="button" @click="closeAgreement">关闭</button>
      <button class="btn btn-primary" type="button" :disabled="!agreementRead"
              @click="finishAgreement">
        {{ agreementRead ? '完成' : '阅读完成后可勾选' }}
      </button>
    </div>
  </Modal>
</template>

<script setup>
import { onMounted, onUnmounted, ref } from 'vue'
import Modal from './Modal.vue'
import FaceAgreement from './FaceAgreement.vue'
import FaceCapture from './FaceCapture.vue'
import { api } from '../api'

const enabled = ref(true)
const faces = ref([])
const consent = ref(false)
const agreementOpen = ref(false)
const agreementRead = ref(false)
const agreementSeconds = ref(0)
const agreementScrolled = ref(false)
const agreementScroll = ref(null)
let agreementTimer = null
const mode = ref('idle')
const capturePurpose = ref('probe')
const session = ref(null)
const bindTicket = ref('')
const emailHint = ref('')
const code = ref('')
const captureError = ref('')
const uploading = ref(false)
const busy = ref(false)
const error = ref('')
const notice = ref('')

function formatDate(value) {
  if (!value) return '-'
  return String(value).replace('T', ' ').replace('Z', '').slice(0, 16)
}

async function load() {
  error.value = ''
  try {
    const [status, list] = await Promise.all([
      api('/api/account/faces/status'),
      api('/api/account/faces')
    ])
    enabled.value = !!status.enabled
    faces.value = list.faces || []
  } catch (e) {
    error.value = e.message
  }
}

async function createSession(purpose) {
  const data = await api('/api/account/faces/session', {
    method: 'POST',
    body: { purpose }
  })
  session.value = data
  capturePurpose.value = purpose
  mode.value = 'capture'
}

async function startProbe() {
  if (!consent.value) {
    error.value = '请先阅读并同意人脸识别服务协议'
    return
  }
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    await createSession('probe')
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function startEnroll() {
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    await createSession('enroll')
    notice.value = '请按提示重新采集。'
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function uploadFrames(url, frames) {
  const form = new FormData()
  form.append('session_token', session.value?.session_token || '')
  frames.forEach((frame, index) => {
    form.append('frames', frame, `frame-${index}.jpg`)
  })
  return api(url, { method: 'POST', body: form })
}

async function onCaptured(frames) {
  if (!frames?.length) {
    captureError.value = '没有采集到足够的清晰画面：请放慢动作、面向光源，保持整张脸在框内后重试。'
    return
  }
  uploading.value = true
  error.value = ''
  notice.value = ''
  captureError.value = ''
  try {
    if (capturePurpose.value === 'probe') {
      const data = await uploadFrames('/api/account/faces/probe', frames)
      session.value = null
      if (data?.already_linked) {
        notice.value = '这张人脸已经绑定到当前账号。'
        mode.value = 'idle'
        await load()
      } else if (data?.matched) {
        bindTicket.value = data.bind_ticket || ''
        emailHint.value = data.email_hint || ''
        mode.value = 'question'
      } else {
        notice.value = '这张人脸尚未登记，接下来进行登记。'
        await startEnroll()
      }
    } else {
      await uploadFrames('/api/account/faces/enroll', frames)
      session.value = null
      mode.value = 'idle'
      notice.value = '人脸登记成功。'
      await load()
    }
  } catch (e) {
    session.value = null
    mode.value = 'capture'
    captureError.value = e.message
  } finally {
    uploading.value = false
  }
}

async function sendBindCode() {
  busy.value = true
  error.value = ''
  notice.value = ''
  try {
    const data = await api('/api/account/faces/send-code', {
      method: 'POST',
      body: { bind_ticket: bindTicket.value }
    })
    emailHint.value = data?.email_hint || emailHint.value
    mode.value = 'email'
    notice.value = '验证码已发送到当前账号邮箱。'
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function confirmBind() {
  busy.value = true
  error.value = ''
  notice.value = ''
  try {
    await api('/api/account/faces/confirm', {
      method: 'POST',
      body: { bind_ticket: bindTicket.value, code: code.value }
    })
    code.value = ''
    bindTicket.value = ''
    mode.value = 'idle'
    notice.value = '已关联到当前账号。'
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function removeFace(face) {
  if (!window.confirm('解除后，该人脸将不能用于当前账号的二次验证；如果没有其他账号关联，也会从人脸服务中删除。确定吗？')) return
  busy.value = true
  error.value = ''
  notice.value = ''
  try {
    await api(`/api/account/faces/${face.id}`, { method: 'DELETE' })
    notice.value = '已解除人脸绑定。'
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

function resetFlow() {
  mode.value = 'idle'
  session.value = null
  bindTicket.value = ''
  code.value = ''
  captureError.value = ''
}

async function retryCapture() {
  captureError.value = ''
  busy.value = true
  try {
    await createSession(capturePurpose.value)
  } catch (e) {
    captureError.value = e.message
  } finally {
    busy.value = false
  }
}

function checkAgreement() {
  if (agreementRead.value) return
  if (agreementSeconds.value >= 5 && agreementScrolled.value) {
    agreementRead.value = true
    consent.value = true
    if (agreementTimer) {
      clearInterval(agreementTimer)
      agreementTimer = null
    }
  }
}

function openAgreement() {
  agreementOpen.value = true
  if (agreementRead.value) return
  agreementSeconds.value = 0
  agreementScrolled.value = false
  if (agreementTimer) clearInterval(agreementTimer)
  agreementTimer = setInterval(() => {
    agreementSeconds.value += 1
    checkAgreement()
  }, 1000)
}

function closeAgreement() {
  agreementOpen.value = false
  if (agreementTimer) {
    clearInterval(agreementTimer)
    agreementTimer = null
  }
}

function finishAgreement() {
  if (agreementRead.value) closeAgreement()
}

function onAgreementScroll(event) {
  if (event.target.scrollTop > 8) {
    agreementScrolled.value = true
    checkAgreement()
  }
}

onMounted(load)
onUnmounted(() => {
  if (agreementTimer) clearInterval(agreementTimer)
})
</script>

<style scoped>
.capture-error {
  padding: 8px 0 4px;
}
.capture-error-title {
  margin: 0 0 8px;
  color: var(--danger);
  font-size: 13px;
  line-height: 1.7;
}
.capture-error-tip {
  margin: 0 0 16px;
  color: var(--ink-soft);
  font-size: 12.5px;
  line-height: 1.75;
}
.capture-lead {
  margin: 0 0 14px;
  font-size: 12.5px;
  line-height: 1.7;
  color: var(--ink-faint);
}
.face-flow {
  padding: 16px 0 4px;
}
.face-flow-title {
  margin: 0 0 8px;
  font-size: 14px;
  font-weight: 600;
  color: var(--ink);
}
.face-flow-lead {
  margin: 0 0 14px;
  font-size: 12.5px;
  line-height: 1.75;
  color: var(--ink-soft);
}
.face-flow-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.face-code-form {
  display: flex;
  gap: 8px;
  margin-bottom: 12px;
}
.face-code-form .form-input {
  max-width: 200px;
}
.text-link {
  padding: 0;
  border: 0;
  background: transparent;
  color: var(--ink-faint);
  font: inherit;
  font-size: 12px;
  text-decoration: underline;
  cursor: pointer;
}
.face-recommend {
  padding: 4px 0;
}
.face-recommend h3 {
  margin: 0 0 8px;
  font-size: 14px;
  font-weight: 600;
  color: var(--ink);
}
.face-recommend p {
  margin: 0 0 8px;
  font-size: 12.5px;
  line-height: 1.75;
  color: var(--ink-soft);
}
.face-recommend-meta {
  color: var(--ink-faint) !important;
  font-size: 11.5px !important;
}
.consent-row {
  display: flex;
  align-items: flex-start;
  gap: 8px;
  margin: 12px 0;
  font-size: 12px;
  line-height: 1.65;
  color: var(--ink-soft);
}
.consent-row input {
  margin-top: 2px;
}
.face-consent-hint {
  margin-top: 8px;
  font-size: 11.5px;
  color: var(--ink-faint);
}
.agreement-link {
  padding: 0;
  border: 0;
  background: transparent;
  color: var(--ink);
  font: inherit;
  text-decoration: underline;
  text-underline-offset: 3px;
  cursor: pointer;
}
.agreement-status {
  margin-bottom: 10px;
  font-size: 12px;
  color: var(--ink-faint);
}
.agreement-scroll {
  max-height: 52vh;
  overflow-y: auto;
  padding: 2px 14px 2px 0;
  border-top: 1px solid var(--line);
  border-bottom: 1px solid var(--line);
}
.face-list {
  margin-bottom: 4px;
}
@media (max-width: 720px) {
  .face-code-form {
    flex-direction: column;
  }
  .face-code-form .form-input {
    max-width: none;
  }
  .agreement-scroll {
    max-height: 55vh;
    padding-right: 8px;
  }
  .face-flow-actions .btn,
  .face-recommend + .consent-row + .face-consent-hint + .btn {
    width: 100%;
  }
}
</style>
