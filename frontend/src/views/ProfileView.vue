<template>
  <div>
    <div class="page-head">
      <div>
        <h1 class="page-title">个人中心</h1>
        <p class="page-sub">当前账号、关联与授权。</p>
      </div>
    </div>

    <div v-if="error" class="alert alert-danger">{{ error }}</div>
    <div v-if="notice" class="alert alert-success">{{ notice }}</div>



    <div v-if="user" class="panel">
      <section class="section">
        <div class="section-head">
          <div>
            <h2 class="section-title">基本信息</h2>
            <p class="section-sub">当前登录账号，邮箱已验证。</p>
          </div>
          <span class="badge badge-secondary">当前账号</span>
        </div>

        <div class="profile-row">
          <div class="avatar avatar-xl">
            <img v-if="user.avatar" :src="user.avatar" alt="" />
            <template v-else>{{ initial }}</template>
          </div>
          <div class="profile-id">
            <div class="profile-email">{{ user.email }}</div>
            <div class="profile-name">{{ user.full_name || user.username || '未设置昵称' }}</div>
            <div class="profile-actions">
              <label class="btn btn-secondary btn-sm" style="cursor: pointer;">
                <input ref="fileInput" type="file" accept="image/*" style="display: none;" @change="uploadAvatar" />
                {{ uploading ? '上传中…' : '更换头像' }}
              </label>
              <button class="btn btn-secondary btn-sm" type="button" @click="openProfileEdit">
                编辑资料
              </button>
            </div>
          </div>
        </div>

        <dl class="kv-grid">
          <div class="kv"><dt>邮箱</dt><dd>{{ user.email }}</dd></div>
          <div class="kv">
            <dt>邮箱验证</dt>
            <dd>
              <span class="badge" :class="user.email_verified ? 'badge-success' : 'badge-secondary'">
                {{ user.email_verified ? '已验证' : '未验证' }}
              </span>
            </dd>
          </div>
          <div class="kv"><dt>显示名</dt><dd>{{ user.full_name || user.username || '未设置' }}</dd></div>
          <div class="kv"><dt>学号</dt><dd>{{ user.student_id || '未设置' }}</dd></div>
          <div class="kv">
            <dt>角色</dt>
            <dd>
              <span v-for="role in user.roles" :key="role.name" class="badge badge-secondary">{{ role.name }}</span>
              <span v-if="!user.roles?.length" class="text-muted">无</span>
            </dd>
          </div>
        </dl>
      </section>

      <SessionsSection />

      <section class="section">
        <div class="section-head">
          <div>
            <h2 class="section-title">登录保护</h2>
            <p class="section-sub">新设备或账号长期未登录时，额外要求一次验证。</p>
          </div>
          <button class="btn btn-secondary btn-sm" type="button"
                  :disabled="securityBusy" @click="toggleStepUp">
            {{ user.step_up_enabled ? '关闭登录保护' : '开启登录保护' }}
          </button>
        </div>
        <div v-if="!user.step_up_enabled" class="alert alert-warning">
          登录保护已关闭（系统默认）：新设备或长期未登录只凭密码即可登录，账号被盗风险会明显增加。
          建议开启，可以使用邮箱、人脸或已登录设备确认。
        </div>
        <p v-else class="section-sub" style="margin: 0 0 14px;">
          已开启。新设备登录时，除了密码，还需要邮箱验证码、人脸或已登录设备确认。
        </p>
      </section>

      <section class="section">
        <div class="section-head">
          <div>
            <h2 class="section-title">关联账号</h2>
            <p class="section-sub">不分主次，可关联多个；一处关联，对方也会显示。</p>
          </div>
          <button class="btn btn-secondary btn-sm" type="button" @click="openLinkModal">关联其他账号</button>
        </div>

        <div v-if="linkedAccounts.length" class="table-wrap">
          <table class="table">
            <thead><tr><th>账号</th><th>姓名</th><th>关联时间</th><th>操作</th></tr></thead>
            <tbody>
              <tr v-for="account in linkedAccounts" :key="account.link_id">
                <td class="cell-main">{{ account.email }}</td>
                <td>{{ account.full_name || account.username || '—' }}</td>
                <td class="text-secondary">{{ formatDate(account.linked_at) }}</td>
                <td><button class="btn btn-danger btn-sm" type="button" @click="unbindLink(account)">解除关联</button></td>
              </tr>
            </tbody>
          </table>
        </div>

        <div v-else class="compact-empty">
          尚未关联其他账号。关联后授权时可以选择以哪个账号登录，权限互不合并。
        </div>
      </section>

      <FaceSection />

      <section class="section">
        <div class="section-head">
          <div>
            <h2 class="section-title">外部身份</h2>
            <p class="section-sub">集市等外部账号。</p>
          </div>
          <button class="btn btn-secondary btn-sm" type="button" @click="startSseBind">绑定集市身份</button>
        </div>

        <div v-if="identities.length" class="table-wrap">
          <table class="table">
            <thead>
              <tr><th>平台</th><th>外部账号</th><th>绑定时间</th><th>操作</th></tr>
            </thead>
            <tbody>
              <tr v-for="identity in identities" :key="identity.id">
                <td><span class="badge badge-secondary">{{ providerLabel(identity.provider) }}</span></td>
                <td>
                  <div class="cell-main">{{ identity.provider_username || identity.provider_user_id }}</div>
                  <div v-if="identity.provider_email" class="cell-sub">{{ identity.provider_email }}</div>
                </td>
                <td class="text-secondary">{{ formatDate(identity.created_at) }}</td>
                <td>
                  <button class="btn btn-danger btn-sm" @click="unbind(identity)">解绑</button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
        <div v-else class="compact-empty">暂无外部身份。</div>
      </section>

      <section class="section">
        <div class="section-head">
          <div>
            <h2 class="section-title">我的授权</h2>
            <p class="section-sub">查看并撤销应用授权。</p>
          </div>
          <RouterLink to="/consents" class="btn btn-secondary btn-sm">管理授权 →</RouterLink>
        </div>
      </section>
    </div>

    <Modal :open="linkModalOpen" @close="linkModalOpen = false">
      <template #title>关联其他账号</template>
      <p class="modal-hint">对方账号会同时看到你；可关联多个，账号不分主次，权限各自独立。</p>
      <div v-if="linkError" class="alert alert-danger">{{ linkError }}</div>
      <div v-if="linkNotice" class="alert alert-success">{{ linkNotice }}</div>
      <form @submit.prevent="bindLink">
        <div class="form-group">
          <label class="form-label">对方账号邮箱 *</label>
          <input v-model.trim="linkEmail" class="form-input" type="email" required
                 placeholder="other@example.com" />
        </div>
        <div class="form-group">
          <label class="form-label">对方密码（与验证码二选一）</label>
          <input v-model="linkPassword" class="form-input" type="password"
                 placeholder="对方账号密码" autocomplete="off" />
        </div>
        <div class="form-group">
          <label class="form-label">邮箱验证码</label>
          <div class="inline-field">
            <input v-model.trim="linkCode" class="form-input" inputmode="numeric"
                   placeholder="6 位验证码" maxlength="6" />
            <button class="btn btn-secondary" type="button" :disabled="linkSending || !linkEmail"
                    @click="sendLinkCode">
              {{ linkSending ? '发送中…' : '发送验证码' }}
            </button>
          </div>
        </div>
        <div class="modal-actions">
          <button class="btn btn-ghost" type="button" @click="linkModalOpen = false">取消</button>
          <button class="btn btn-primary" type="submit" :disabled="linkSubmitting">
            {{ linkSubmitting ? '关联中…' : '确认关联' }}
          </button>
        </div>
      </form>
    </Modal>

    <Modal :open="profileEditOpen" @close="profileEditOpen = false">
      <template #title>编辑资料</template>
      <div v-if="profileError" class="alert alert-danger">{{ profileError }}</div>
      <form @submit.prevent="saveProfile">
        <div class="form-group">
          <label class="form-label">显示名</label>
          <input v-model.trim="profileForm.full_name" class="form-input" maxlength="100"
                 placeholder="例如：张三" />
        </div>
        <div class="form-group">
          <label class="form-label">学号</label>
          <input v-model.trim="profileForm.student_id" class="form-input" maxlength="64"
                 placeholder="例如：20260001" />
        </div>
        <div class="form-group">
          <label class="form-label">邮箱</label>
          <input :value="user?.email || ''" class="form-input" disabled />
        </div>
        <p class="modal-hint">邮箱和角色不能在这里修改。显示名和学号会同步到 OAuth 返回的资料里。</p>
        <div class="modal-actions">
          <button class="btn btn-ghost" type="button" @click="profileEditOpen = false">取消</button>
          <button class="btn btn-primary" type="submit" :disabled="profileSaving">
            {{ profileSaving ? '保存中…' : '保存' }}
          </button>
        </div>
      </form>
    </Modal>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { useRoute } from 'vue-router'
import Modal from '../components/Modal.vue'
import FaceSection from '../components/FaceSection.vue'
import SessionsSection from '../components/SessionsSection.vue'
import { api } from '../api'
import { store, loadMe } from '../store'

const error = ref('')
const notice = ref('')
const uploading = ref(false)
const fileInput = ref(null)
const profileEditOpen = ref(false)
const profileSaving = ref(false)
const securityBusy = ref(false)
const profileError = ref('')
const profileForm = reactive({ full_name: '', student_id: '' })
const user = computed(() => store.user)
const identities = computed(() => store.user?.identities || [])
const initial = computed(() => {
  const u = store.user
  return ((u?.full_name || u?.username || u?.email || '?').slice(0, 1) || '?').toUpperCase()
})

const route = useRoute()

const linkModalOpen = ref(false)
const linkEmail = ref('')
const linkPassword = ref('')
const linkCode = ref('')
const linkSending = ref(false)
const linkSubmitting = ref(false)
const linkError = ref('')
const linkNotice = ref('')
const linkedAccounts = computed(() => store.user?.linked_accounts || [])

function openProfileEdit() {
  profileForm.full_name = store.user?.full_name || ''
  profileForm.student_id = store.user?.student_id || ''
  profileError.value = ''
  profileEditOpen.value = true
}

async function saveProfile() {
  profileError.value = ''
  profileSaving.value = true
  try {
    await api('/api/me/profile', {
      method: 'PUT',
      body: {
        full_name: profileForm.full_name,
        student_id: profileForm.student_id
      }
    })
    await loadMe(true)
    profileEditOpen.value = false
    notice.value = '资料已更新。'
  } catch (e) {
    profileError.value = e.message
  } finally {
    profileSaving.value = false
  }
}

async function toggleStepUp() {
  const next = !store.user?.step_up_enabled
  if (!next && !confirm('关闭登录保护后，新设备或长期未登录只凭密码即可登录，账号被盗风险会明显增加。确定关闭吗？')) {
    return
  }
  error.value = ''
  notice.value = ''
  securityBusy.value = true
  try {
    await api('/api/me/profile', {
      method: 'PUT',
      body: { step_up_enabled: next }
    })
    await loadMe(true)
    notice.value = next ? '登录保护已开启。' : '登录保护已关闭。'
  } catch (e) {
    error.value = e.message
  } finally {
    securityBusy.value = false
  }
}

function openLinkModal() {
  linkEmail.value = ''
  linkPassword.value = ''
  linkCode.value = ''
  linkError.value = ''
  linkNotice.value = ''
  linkModalOpen.value = true
}

async function sendLinkCode() {
  linkError.value = ''
  linkNotice.value = ''
  linkSending.value = true
  try {
    await api('/api/account/links/send-code', {
      method: 'POST',
      body: { email: linkEmail.value }
    })
    linkNotice.value = '验证码已发送到该邮箱。'
  } catch (e) {
    linkError.value = e.message
  } finally {
    linkSending.value = false
  }
}

async function bindLink() {
  linkError.value = ''
  linkNotice.value = ''
  linkSubmitting.value = true
  try {
    await api('/api/account/links', {
      method: 'POST',
      body: { email: linkEmail.value, password: linkPassword.value, code: linkCode.value }
    })
    linkModalOpen.value = false
    await loadMe(true)
    notice.value = '关联账号成功。'
  } catch (e) {
    linkError.value = e.message
  } finally {
    linkSubmitting.value = false
  }
}

async function unbindLink(account) {
  if (!confirm(`确定要解除与 ${account.email} 的关联吗？`)) return
  error.value = ''
  try {
    await api('/api/account/links/' + account.link_id, { method: 'DELETE' })
    await loadMe(true)
    notice.value = '已解除关联。'
  } catch (e) {
    error.value = e.message
  }
}

function providerLabel(provider) {
  const labels = { sse: '集市账号', ssemarket: '集市账号', github: 'GitHub', wechat: '微信', custom: '其他' }
  return labels[provider] || provider
}

async function startSseBind() {
  error.value = ''
  notice.value = ''
  try {
    const data = await api('/api/auth/sse/bind/start')
    if (data?.authorize_url) {
      window.location.assign(data.authorize_url)
    } else {
      error.value = '绑定入口暂不可用'
    }
  } catch (e) {
    error.value = e.message
  }
}

onMounted(() => {
  if (route.query.bind === 'success') notice.value = '集市账号绑定成功。'
  else if (route.query.bind === 'conflict') error.value = '该集市账号已绑定其他账号。'
})

function formatDate(value) {
  if (!value) return '-'
  return String(value).replace('T', ' ').replace('Z', '').slice(0, 16)
}

async function uploadAvatar(event) {
  const file = event.target.files?.[0]
  if (!file) return
  if (file.size > 5 * 1024 * 1024) {
    error.value = '头像不能超过 5MB'
    return
  }
  error.value = ''
  uploading.value = true
  try {
    const form = new FormData()
    form.append('file', file)
    await api('/api/me/avatar', { method: 'POST', body: form })
    await loadMe(true)
  } catch (e) {
    error.value = e.message
  } finally {
    uploading.value = false
    if (fileInput.value) fileInput.value.value = ''
  }
}

async function unbind(identity) {
  if (!confirm(`确定要解绑 ${identity.provider} 账号吗？`)) return
  error.value = ''
  try {
    await api('/api/identities/' + identity.id, { method: 'DELETE' })
    await loadMe(true)
  } catch (e) {
    error.value = e.message
  }
}
</script>

<style scoped>
.security-tip {
  margin-bottom: 18px;
  padding: 12px 14px;
  border-left: 2px solid var(--line-strong);
  font-size: 12.5px;
  line-height: 1.75;
  color: var(--ink-faint);
}
.security-tip strong {
  color: var(--ink);
  font-weight: 600;
  margin-right: 6px;
}
.modal-hint {
  margin: 0 0 16px;
  font-size: 12.5px;
  line-height: 1.7;
  color: var(--ink-faint);
}
.inline-field {
  display: flex;
  gap: 8px;
}
.inline-field .form-input {
  flex: 1;
  min-width: 0;
}
.profile-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 12px;
}
</style>
