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

    <div v-if="user">
      <section class="phero">
        <div class="phero-id">
          <div class="avatar avatar-xl">
            <img v-if="user.avatar" :src="user.avatar" alt="" />
            <template v-else>{{ initial }}</template>
          </div>
          <div class="phero-text">
            <div class="phero-name">{{ user.full_name || user.username || '未设置昵称' }}</div>
            <div class="phero-mail">{{ user.email }}</div>
            <div class="phero-meta">
              <span v-if="user.is_internal" class="badge badge-success">学院成员</span>
              <span v-if="user.is_super_admin" class="badge badge-primary">超级管理员</span>
              <span class="badge" :class="user.email_verified ? 'badge-success' : 'badge-secondary'">
                {{ user.email_verified ? '邮箱已验证' : '邮箱未验证' }}
              </span>
            </div>
          </div>
        </div>
        <div class="phero-actions">
          <label class="btn btn-secondary btn-sm" style="cursor: pointer;">
            <input ref="fileInput" type="file" accept="image/*" style="display: none;" @change="uploadAvatar" />
            {{ uploading ? '上传中…' : '更换头像' }}
          </label>
          <button class="btn btn-secondary btn-sm" type="button" @click="openProfileEdit">编辑资料</button>
          <RouterLink class="btn btn-secondary btn-sm" to="/reset-password">修改密码</RouterLink>
        </div>
      </section>

      <section class="pmeta">
        <div class="pmeta-cell">
          <span class="pmeta-label">显示名</span>
          <span class="pmeta-value">{{ user.full_name || user.username || '未设置' }}</span>
        </div>
        <div class="pmeta-cell">
          <span class="pmeta-label">学号</span>
          <span class="pmeta-value">{{ user.student_id || '未设置' }}</span>
        </div>
        <div class="pmeta-cell">
          <span class="pmeta-label">已授权应用</span>
          <span class="pmeta-value">{{ consents.length }}</span>
        </div>
      </section>

      <div class="pgrid">
        <div class="pgrid-main">
          <section class="psec">
            <div class="section-head">
              <div>
                <h2 class="section-title">授权的应用</h2>
                <p class="section-sub">这些应用获得的权限，可随时撤销。</p>
              </div>
            </div>
            <div v-if="consents.length" class="pstack">
              <div v-for="consent in consents" :key="consent.id" class="prow">
                <img v-if="consent.application?.icon" :src="consent.application.icon" class="app-icon" alt="" />
                <span v-else class="app-icon">{{ (consent.application?.name || '?').slice(0, 1) }}</span>
                <div class="prow-main">
                  <div class="prow-title">{{ consent.application?.name || consent.application?.client_id }}</div>
                  <div class="prow-sub">
                    <span v-for="scope in consent.scopes" :key="scope" class="tag">{{ scope }}</span>
                  </div>
                </div>
                <div class="prow-side">
                  <span class="prow-date">{{ formatDate(consent.granted_at) }} 授权</span>
                  <button class="btn btn-danger btn-sm" type="button" @click="revokeConsent(consent)">撤销授权</button>
                </div>
              </div>
            </div>
            <div v-else class="compact-empty">尚未授权任何第三方应用。</div>
          </section>

          <section class="psec">
            <div class="section-head">
              <div>
                <h2 class="section-title">我的接入应用</h2>
                <p class="section-sub">登录后即可自助注册对外授权应用，提交即生效（无需审核；每个账号至多 20 个）。</p>
              </div>
              <button class="btn btn-primary btn-sm" type="button" @click="openApply">注册接入应用</button>
            </div>
            <div v-if="myApps.length" class="pstack">
              <div v-for="app in myApps" :key="app.id" class="prow">
                <div class="prow-main">
                  <div class="prow-title">{{ app.name }}</div>
                  <div v-if="app.description" class="prow-desc">{{ app.description }}</div>
                  <div class="prow-sub"><code class="code-inline">{{ app.client_id }}</code></div>
                </div>
                <div class="prow-side prow-side-badges">
                  <span class="badge" :class="app.kind === 'external' ? 'badge-secondary' : 'badge-primary'">
                    {{ app.kind === 'external' ? '对外' : '内接' }}
                  </span>
                  <span class="badge" :class="statusClass(app.status)">{{ statusLabel(app.status) }}</span>
                  <button class="btn btn-secondary btn-sm" type="button" @click="resetSecret(app)">重置 Secret</button>
                  <button class="btn btn-danger btn-sm" type="button" @click="removeApp(app)">删除</button>
                </div>
              </div>
            </div>
            <div v-else class="compact-empty">还没有注册过接入应用。</div>
          </section>
        </div>

      </div>

      <div class="psessions">
        <SessionsSection />
      </div>
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
      <template #title>注册接入应用（对外授权）</template>
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
        <p class="modal-hint">提交后立即生效，并发放接入凭据（client_id / client_secret，只显示一次）；每个账号至多 20 个应用。</p>
        <div class="modal-actions">
          <button class="btn btn-ghost" type="button" @click="applyOpen = false">取消</button>
          <button class="btn btn-primary" type="submit" :disabled="applySubmitting">
            {{ applySubmitting ? '注册中…' : '注册' }}
          </button>
        </div>
      </form>
    </Modal>

    <Modal :open="secretOpen" @close="secretOpen = false">
      <template #title>{{ secretInfo.title }}</template>
      <p class="page-sub">请立即保存以下凭证。Client Secret 只会显示这一次。</p>
      <div class="form-group">
        <label class="form-label">Client ID</label>
        <div class="secret-box">{{ secretInfo.clientId }}</div>
      </div>
      <div v-if="secretInfo.clientSecret" class="form-group">
        <label class="form-label">Client Secret</label>
        <div class="secret-box">{{ secretInfo.clientSecret }}</div>
      </div>
      <div class="modal-actions">
        <button class="btn btn-primary" type="button" @click="secretOpen = false">我已保存</button>
      </div>
    </Modal>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
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
const initial = computed(() => {
  const u = store.user
  return ((u?.full_name || u?.username || u?.email || '?').slice(0, 1) || '?').toUpperCase()
})

const scopeCatalog = [
  { name: 'openid', description: '基础身份标识' },
  { name: 'profile:username', description: '用户昵称' },
  { name: 'profile:name', description: '真实姓名' },
  { name: 'profile:avatar', description: '头像' },
  { name: 'profile:student_id', description: '学号' },
  { name: 'email', description: '邮箱地址' },
  { name: 'roles', description: '用户角色列表' },
  { name: 'sse_identity', description: '集市身份标识（sse_sub，可跨服务关联同一人）' }
]

const consents = ref([])
const myApps = ref([])
const applyOpen = ref(false)
const applySubmitting = ref(false)
const applyError = ref('')
const secretOpen = ref(false)
const secretInfo = reactive({ title: '', clientId: '', clientSecret: '' })
const applyForm = reactive({
  name: '',
  description: '',
  homepageUrl: '',
  redirectUris: '',
  allowedScopes: ['openid']
})

function statusLabel(status) {
  const labels = { approved: '已生效', pending: '待审核', rejected: '已驳回', suspended: '已暂停' }
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
    const data = await api('/api/apps/apply', {
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
    secretInfo.title = '应用已注册（立即生效）'
    secretInfo.clientId = data?.client_id || ''
    secretInfo.clientSecret = data?.client_secret || ''
    secretOpen.value = true
    notice.value = '应用已注册并生效，请保存好接入凭据。'
    await loadPermissions()
  } catch (e) {
    applyError.value = e.message
  } finally {
    applySubmitting.value = false
  }
}

async function resetSecret(app) {
  if (!confirm(`重置「${app.name}」的 Client Secret？旧 secret 将立即失效。`)) return
  error.value = ''
  try {
    const data = await api('/api/apps/' + app.id + '/reset-secret', { method: 'POST' })
    secretInfo.title = 'Secret 已重置'
    secretInfo.clientId = app.client_id || ''
    secretInfo.clientSecret = data?.client_secret || ''
    secretOpen.value = true
  } catch (e) {
    error.value = e.message
  }
}

async function removeApp(app) {
  if (!confirm(`删除应用「${app.name}」？此操作不可撤销。`)) return
  error.value = ''
  try {
    await api('/api/apps/' + app.id, { method: 'DELETE' })
    notice.value = '应用已删除。'
    await loadPermissions()
  } catch (e) {
    error.value = e.message
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


onMounted(() => {
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

</script>

<style scoped>
/* Identity hero. */
.phero {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 24px;
  padding: 26px 0 22px;
  border-bottom: 1px solid var(--line);
}
.phero-id { display: flex; align-items: center; gap: 18px; min-width: 0; }
.phero-name {
  font-size: 26px;
  font-weight: 700;
  letter-spacing: -.045em;
  line-height: 1.15;
}
.phero-mail { margin-top: 3px; font-size: 12.5px; color: var(--ink-soft); word-break: break-all; }
.phero-meta { display: flex; flex-wrap: wrap; gap: 7px; margin-top: 10px; }
.phero-actions { display: flex; flex-wrap: wrap; gap: 8px; flex: 0 0 auto; }

/* Meta strip. */
.pmeta {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  border-bottom: 1px solid var(--line);
}
.pmeta-cell { padding: 14px 18px; border-right: 1px solid var(--line-soft); min-width: 0; }
.pmeta-cell:first-child { padding-left: 0; }
.pmeta-cell:last-child { border-right: 0; }
.pmeta-label {
  display: block;
  font-size: 9.5px;
  font-weight: 600;
  letter-spacing: .14em;
  text-transform: uppercase;
  color: var(--ink-faint);
}
.pmeta-value { display: block; margin-top: 5px; font-size: 12.5px; color: var(--ink); word-break: break-all; }

/* Sections stack full width. */
.pgrid-main { padding: 4px 0 0; min-width: 0; }

.psec { padding: 22px 0; }
.psec + .psec { border-top: 1px solid var(--line-soft); }
.psec .section-head { margin-bottom: 12px; }

/* Rows. */
.pstack { border-top: 1px solid var(--line); }
.prow {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 13px 0;
  border-bottom: 1px solid var(--line-soft);
}
.prow:last-child { border-bottom: 0; }
.prow > .app-icon { flex: 0 0 30px; }
.prow-main { flex: 1; min-width: 0; }
.prow-title { font-size: 12.5px; font-weight: 600; color: var(--ink); }
.prow-sub { display: flex; flex-wrap: wrap; gap: 4px; margin-top: 5px; }
.prow-desc { margin-top: 2px; font-size: 11px; color: var(--ink-soft); }
.prow-date { margin-top: 2px; font-size: 10.5px; color: var(--ink-faint); }
.prow-side { display: flex; flex-direction: column; align-items: flex-end; gap: 6px; flex: 0 0 auto; }
.prow-side-badges { flex-direction: row; align-items: center; flex-wrap: wrap; justify-content: flex-end; }

/* Sessions sits full width under the sections. */
.psessions { margin-top: 4px; }

@media (max-width: 900px) {
  .phero { align-items: flex-start; flex-direction: column; gap: 14px; padding: 22px 0 18px; }
  .phero-name { font-size: 24px; }
  .phero-actions { width: 100%; }
  .pmeta { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .pmeta-cell { padding: 12px 14px; }
  .pmeta-cell:nth-child(2n) { border-right: 0; }
  .pmeta-cell:nth-child(n+3) { border-top: 1px solid var(--line-soft); }
  .pmeta-cell:first-child { padding-left: 14px; }
  .prow-side { flex-direction: row; align-items: center; }
  .prow { flex-wrap: wrap; }
}
</style>
