<template>
  <div>
    <div class="page-head">
      <div>
        <h1 class="page-title">个人中心</h1>
        <p class="page-sub">身份、权限与安全。</p>
      </div>
    </div>

    <div v-if="error" class="alert alert-danger">{{ error }}</div>
    <div v-if="notice" class="alert alert-success">{{ notice }}</div>

    <div v-if="user" class="panel">
      <section class="section">
        <div class="section-head">
          <div>
            <h2 class="section-title">我的身份</h2>
            <p class="section-sub">当前登录账号。</p>
          </div>
          <div class="tag-row">
            <span v-if="user.is_internal" class="badge badge-success">学院成员</span>
            <span v-if="user.is_super_admin" class="badge badge-primary">超级管理员</span>
          </div>
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
              <RouterLink class="btn btn-secondary btn-sm" to="/reset-password">修改密码</RouterLink>
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
        </dl>

        <div class="bind-block">
          <div class="section-head">
            <div>
              <h3 class="section-title bind-title">绑定的集市账号</h3>
              <p class="section-sub">绑定后可用集市身份登录。</p>
            </div>
            <button class="btn btn-secondary btn-sm" type="button" @click="startSseBind">绑定集市账号</button>
          </div>
          <div v-if="identities.length" class="table-wrap">
            <table class="table">
              <thead>
                <tr><th>账号</th><th>绑定时间</th><th>操作</th></tr>
              </thead>
              <tbody>
                <tr v-for="identity in identities" :key="identity.id">
                  <td>
                    <div class="cell-main">{{ identity.provider_username || identity.provider_user_id }}</div>
                    <div v-if="identity.provider_email" class="cell-sub">{{ identity.provider_email }}</div>
                  </td>
                  <td class="text-secondary">{{ formatDate(identity.created_at) }}</td>
                  <td><button class="btn btn-danger btn-sm" @click="unbind(identity)">解绑</button></td>
                </tr>
              </tbody>
            </table>
          </div>
          <div v-else class="compact-empty">暂无绑定的集市账号。</div>
        </div>
      </section>

      <section class="section">
        <div class="section-head">
          <div>
            <h2 class="section-title">我的权限</h2>
            <p class="section-sub">第三方应用的授权与自己的接入申请。</p>
          </div>
          <button class="btn btn-primary btn-sm" type="button" @click="openApply">申请接入</button>
        </div>

        <h3 class="section-title block-title">已授权的第三方应用</h3>
        <div v-if="consents.length" class="table-wrap">
          <table class="table">
            <thead><tr><th>应用</th><th>权限</th><th>授权时间</th><th>操作</th></tr></thead>
            <tbody>
              <tr v-for="consent in consents" :key="consent.id">
                <td>
                  <div class="cell-ident">
                    <img v-if="consent.application?.icon" :src="consent.application.icon" class="app-icon" alt="" />
                    <span v-else class="app-icon">{{ (consent.application?.name || '?').slice(0, 1) }}</span>
                    <div class="cell-ident-text">
                      <div class="cell-main">{{ consent.application?.name || consent.application?.client_id }}</div>
                    </div>
                  </div>
                </td>
                <td>
                  <div class="tag-row">
                    <span v-for="scope in consent.scopes" :key="scope" class="tag">{{ scope }}</span>
                  </div>
                </td>
                <td class="text-secondary">{{ formatDate(consent.granted_at) }}</td>
                <td><button class="btn btn-danger btn-sm" type="button" @click="revokeConsent(consent)">撤销授权</button></td>
              </tr>
            </tbody>
          </table>
        </div>
        <div v-else class="compact-empty">尚未授权任何第三方应用。</div>

        <h3 class="section-title block-title">我的接入申请</h3>
        <div v-if="myApps.length" class="table-wrap">
          <table class="table">
            <thead><tr><th>应用</th><th>类型</th><th>状态</th><th>说明</th></tr></thead>
            <tbody>
              <tr v-for="app in myApps" :key="app.id">
                <td>
                  <div class="cell-main">{{ app.name }}</div>
                  <div v-if="app.description" class="cell-sub">{{ app.description }}</div>
                </td>
                <td>
                  <span class="badge" :class="app.kind === 'external' ? 'badge-secondary' : 'badge-primary'">
                    {{ app.kind === 'external' ? '对外' : '内接' }}
                  </span>
                </td>
                <td>
                  <span class="badge" :class="statusClass(app.status)">{{ statusLabel(app.status) }}</span>
                </td>
                <td class="text-secondary">{{ app.review_note || '—' }}</td>
              </tr>
            </tbody>
          </table>
        </div>
        <div v-else class="compact-empty">还没有提交过接入申请。学院成员可申请对外授权应用，由管理员审核。</div>
      </section>

      <SessionsSection />
    </div>

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
        <p class="modal-hint">邮箱不可修改。显示名和学号会同步到应用获取的资料里。</p>
        <div class="modal-actions">
          <button class="btn btn-ghost" type="button" @click="profileEditOpen = false">取消</button>
          <button class="btn btn-primary" type="submit" :disabled="profileSaving">
            {{ profileSaving ? '保存中…' : '保存' }}
          </button>
        </div>
      </form>
    </Modal>

    <Modal :open="applyOpen" @close="applyOpen = false">
      <template #title>申请接入（对外授权应用）</template>
      <div v-if="applyError" class="alert alert-danger">{{ applyError }}</div>
      <form @submit.prevent="submitApply">
        <div class="form-group">
          <label class="form-label">应用名称 *</label>
          <input v-model.trim="applyForm.name" class="form-input" required placeholder="例如：课程表小工具" />
        </div>
        <div class="form-group">
          <label class="form-label">描述</label>
          <input v-model.trim="applyForm.description" class="form-input" placeholder="这个应用做什么" />
        </div>
        <div class="form-group">
          <label class="form-label">主页 URL</label>
          <input v-model.trim="applyForm.homepageUrl" class="form-input" type="url" placeholder="https://..." />
        </div>
        <div class="form-group">
          <label class="form-label">回调地址（每行一个）*</label>
          <textarea v-model="applyForm.redirectUris" class="form-input" rows="3" required
                    placeholder="https://your-app.com/callback"></textarea>
        </div>
        <div class="form-group">
          <label class="form-label">申请的权限</label>
          <div class="check-grid">
            <label v-for="item in scopeCatalog" :key="item.name" class="check-row">
              <input v-model="applyForm.allowedScopes" type="checkbox" :value="item.name" />
              <span>{{ item.description }}</span>
              <code class="check-name">{{ item.name }}</code>
            </label>
          </div>
        </div>
        <p class="modal-hint">仅学院成员可申请；提交后由管理员审核，审核通过才会发放接入凭据。</p>
        <div class="modal-actions">
          <button class="btn btn-ghost" type="button" @click="applyOpen = false">取消</button>
          <button class="btn btn-primary" type="submit" :disabled="applySubmitting">
            {{ applySubmitting ? '提交中…' : '提交申请' }}
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
import SessionsSection from '../components/SessionsSection.vue'
import { api } from '../api'
import { store, loadMe } from '../store'

const error = ref('')
const notice = ref('')
const uploading = ref(false)
const fileInput = ref(null)
const profileEditOpen = ref(false)
const profileSaving = ref(false)
const profileError = ref('')
const profileForm = reactive({ full_name: '', student_id: '' })
const user = computed(() => store.user)
const identities = computed(() => store.user?.identities || [])
const initial = computed(() => {
  const u = store.user
  return ((u?.full_name || u?.username || u?.email || '?').slice(0, 1) || '?').toUpperCase()
})

const route = useRoute()

const scopeCatalog = [
  { name: 'openid', description: '基础身份标识' },
  { name: 'profile:username', description: '用户昵称' },
  { name: 'profile:name', description: '真实姓名' },
  { name: 'profile:avatar', description: '头像' },
  { name: 'profile:student_id', description: '学号' },
  { name: 'email', description: '邮箱地址' }
]

const consents = ref([])
const myApps = ref([])
const applyOpen = ref(false)
const applySubmitting = ref(false)
const applyError = ref('')
const applyForm = reactive({
  name: '',
  description: '',
  homepageUrl: '',
  redirectUris: '',
  allowedScopes: ['openid']
})

function statusLabel(status) {
  const labels = { approved: '已通过', pending: '待审核', rejected: '已驳回', suspended: '已暂停' }
  return labels[status] || status || '—'
}
function statusClass(status) {
  if (status === 'approved') return 'badge-success'
  if (status === 'pending') return 'badge-secondary'
  return 'badge-secondary'
}

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

async function loadPermissions() {
  try {
    const [consentData, appData] = await Promise.all([
      api('/api/consents'),
      api('/api/apps')
    ])
    consents.value = consentData?.consents || []
    myApps.value = (appData?.applications || []).filter((app) => app.kind !== 'internal')
  } catch (e) {
    if (!error.value) error.value = e.message
  }
}

function openApply() {
  applyForm.name = ''
  applyForm.description = ''
  applyForm.homepageUrl = ''
  applyForm.redirectUris = ''
  applyForm.allowedScopes = ['openid']
  applyError.value = ''
  applyOpen.value = true
}

async function submitApply() {
  applyError.value = ''
  applySubmitting.value = true
  try {
    const redirects = applyForm.redirectUris.split('\n').map((line) => line.trim()).filter(Boolean)
    await api('/api/apps/apply', {
      method: 'POST',
      body: {
        name: applyForm.name,
        description: applyForm.description,
        homepage_url: applyForm.homepageUrl || null,
        redirect_uris: redirects,
        allowed_scopes: applyForm.allowedScopes
      }
    })
    applyOpen.value = false
    notice.value = '申请已提交，等待管理员审核。'
    await loadPermissions()
  } catch (e) {
    applyError.value = e.message
  } finally {
    applySubmitting.value = false
  }
}

async function revokeConsent(consent) {
  if (!confirm(`确定要撤销「${consent.application?.name || '该应用'}」的授权吗？撤销后需要重新授权才能使用。`)) return
  error.value = ''
  try {
    await api('/api/consents/' + consent.id, { method: 'DELETE' })
    await loadPermissions()
    notice.value = '已撤销授权。'
  } catch (e) {
    error.value = e.message
  }
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
  loadPermissions()
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
  if (!confirm(`确定要解绑该集市账号吗？`)) return
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
.modal-hint {
  margin: 0 0 16px;
  font-size: 12.5px;
  line-height: 1.7;
  color: var(--ink-faint);
}
.profile-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 12px;
}
.bind-block {
  margin-top: 20px;
  padding-top: 16px;
  border-top: 1px solid var(--line);
}
.bind-title {
  font-size: 13px;
}
.block-title {
  font-size: 13px;
  margin: 18px 0 10px;
}
</style>
