<template>
  <div>
    <div class="page-head">
      <div>
        <h1 class="page-title">应用管理</h1>
        <p class="page-sub">内接平台与对外授权应用，含接入审核。</p>
      </div>
      <div class="page-head-actions">
        <a class="btn btn-secondary" href="/api/apps/integration-skill" download>
          下载接入 Skill
        </a>
        <button class="btn btn-primary" type="button" @click="openCreate">注册内接应用</button>
      </div>
    </div>

    <div class="filter-tabs">
      <button class="filter-tab" :class="{ 'is-active': filter === 'all' }" type="button"
              @click="filter = 'all'">全部 ({{ apps.length }})</button>
      <button class="filter-tab" :class="{ 'is-active': filter === 'pending' }" type="button"
              @click="filter = 'pending'">待审核 ({{ pendingCount }})</button>
    </div>

    <div v-if="error" class="alert alert-danger">{{ error }}</div>
    <div v-if="loading" class="loading-line"><span class="spinner"></span>加载中…</div>

    <div v-else class="panel">
      <div v-if="filteredApps.length" class="table-wrap">
        <table class="table">
          <thead>
            <tr><th>应用</th><th>Client ID</th><th>类型</th><th>审核状态</th><th>状态</th><th>无感登录</th><th>Scopes</th><th>操作</th></tr>
          </thead>
          <tbody>
            <tr v-for="app in filteredApps" :key="app.id">
              <td>
                <div class="cell-ident">
                  <img v-if="app.icon" :src="app.icon" class="app-icon" alt="" />
                  <span v-else class="app-icon">{{ (app.name || '?').slice(0, 1) }}</span>
                  <div class="cell-ident-text">
                    <div class="cell-main">{{ app.name }}</div>
                    <div v-if="app.description" class="cell-sub">{{ app.description }}</div>
                  </div>
                </div>
              </td>
              <td><code class="code-inline">{{ app.client_id }}</code></td>
              <td>
                <span class="badge" :class="app.kind === 'external' ? 'badge-secondary' : 'badge-primary'">
                  {{ app.kind === 'external' ? '对外' : '内接' }}
                </span>
              </td>
              <td>
                <span class="badge" :class="reviewClass(app.status)">{{ reviewLabel(app.status) }}</span>
                <div v-if="app.review_note" class="cell-sub">{{ app.review_note }}</div>
              </td>
              <td>
                <span class="badge" :class="app.is_active ? 'badge-success' : 'badge-secondary'">
                  {{ app.is_active ? '启用' : '禁用' }}
                </span>
              </td>
              <td>
                <span v-if="app.kind !== 'external'" class="badge"
                      :class="app.m2m_enabled ? 'badge-success' : 'badge-secondary'">
                  {{ app.m2m_enabled ? '开' : '关' }}
                </span>
                <span v-else class="text-muted">—</span>
              </td>
              <td>
                <div class="tag-row">
                  <span v-for="scope in app.allowed_scopes" :key="scope" class="tag">{{ scope }}</span>
                </div>
              </td>
              <td>
                <div class="row-actions">
                  <template v-if="app.status === 'pending'">
                    <button class="btn btn-primary btn-sm" type="button"
                            :disabled="reviewBusy" @click="review(app, 'approve')">通过</button>
                    <button class="btn btn-secondary btn-sm" type="button"
                            :disabled="reviewBusy" @click="review(app, 'reject')">驳回</button>
                  </template>
                  <template v-else-if="app.kind === 'external' && app.status === 'approved'">
                    <button class="btn btn-secondary btn-sm" type="button"
                            :disabled="reviewBusy" @click="review(app, 'suspend')">暂停</button>
                  </template>
                  <button class="btn btn-secondary btn-sm" type="button" @click="openDetail(app)">详情</button>
                  <button class="btn btn-danger btn-sm" type="button" @click="remove(app)">删除</button>
                </div>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <div v-else class="empty-state">
        <h3>{{ filter === 'pending' ? '没有待审核的申请' : '还没有接入应用' }}</h3>
        <p>{{ filter === 'pending' ? '' : '用「注册内接应用」添加自有平台。' }}</p>
      </div>
    </div>

    <Modal :open="showCreate" @close="showCreate = false">
      <template #title>注册内接应用</template>
      <form @submit.prevent="createApp">
        <div class="form-group">
          <label class="form-label">应用名称 *</label>
          <input v-model.trim="form.name" class="form-input" required placeholder="例如：SSE Market" />
        </div>
        <div class="form-group">
          <label class="form-label">描述</label>
          <input v-model.trim="form.description" class="form-input" placeholder="应用简介" />
        </div>
        <div class="form-group">
          <label class="form-label">图标 URL</label>
          <input v-model.trim="form.icon" class="form-input" placeholder="https://.../logo.png" />
        </div>
        <div class="form-group">
          <label class="form-label">回调地址（每行一个）*</label>
          <textarea v-model="form.redirectUris" class="form-input" rows="3" required
                    placeholder="https://your-app.com/callback"></textarea>
        </div>
        <div class="form-group">
          <label class="form-label">允许的 Scopes（应用可请求的权限上限）</label>
          <div class="check-grid">
            <label v-for="item in scopeCatalog" :key="item.name" class="check-row">
              <input v-model="form.allowedScopes" type="checkbox" :value="item.name" />
              <span>{{ item.description }}</span>
              <code class="check-name">{{ item.name }}</code>
            </label>
          </div>
        </div>
        <div class="form-group">
          <label class="form-label">主页 URL</label>
          <input v-model.trim="form.homepageUrl" class="form-input" type="url" placeholder="https://your-app.com" />
        </div>
        <div class="form-group" style="margin-bottom: 0;">
          <label class="checkbox-line">
            <input v-model="form.isConfidential" type="checkbox" />
            <span>Confidential 客户端（有 secret）</span>
          </label>
        </div>
        <div class="modal-actions">
          <button class="btn btn-secondary" type="button" @click="showCreate = false">取消</button>
          <button class="btn btn-primary" type="submit" :disabled="submitting">
            {{ submitting ? '创建中…' : '创建' }}
          </button>
        </div>
      </form>
    </Modal>

    <Modal :open="showDetail" @close="showDetail = false">
      <template #title>应用详情</template>
      <div v-if="detail">
        <div class="form-group">
          <label class="form-label">应用名称</label>
          <input :value="detail.name" class="form-input" readonly />
        </div>
        <div v-if="detail.icon" class="form-group">
          <label class="form-label">图标</label>
          <img :src="detail.icon" class="consent-app-logo" alt="" />
        </div>
        <div class="form-group">
          <label class="form-label">Client ID</label>
          <input :value="detail.client_id" class="form-input" readonly />
        </div>
        <div class="form-group">
          <label class="form-label">回调地址</label>
          <textarea :value="(detail.redirect_uris || []).join('\n')" class="form-input" rows="2" readonly></textarea>
        </div>
        <div class="form-group">
          <label class="form-label">Scopes</label>
          <input :value="(detail.allowed_scopes || []).join(' ')" class="form-input" readonly />
        </div>
        <div v-if="detail.kind !== 'external'" class="form-group">
          <label class="form-label">无感登录（M2M 密码校验）</label>
          <label class="checkbox-line">
            <input type="checkbox" :checked="detail.m2m_enabled" :disabled="savingM2m"
                   @change="toggleM2m" />
            <span>允许该应用用 client 凭据调用密码校验接口，实现平台间无感登录</span>
          </label>
        </div>
        <div class="form-group">
          <label class="form-label">状态</label>
          <div class="tag-row">
            <span class="badge" :class="detail.is_active ? 'badge-success' : 'badge-secondary'">
              {{ detail.is_active ? '启用' : '禁用' }}
            </span>
            <span class="badge" :class="detail.is_confidential ? 'badge-primary' : 'badge-secondary'">
              {{ detail.is_confidential ? 'Confidential' : 'Public' }}
            </span>
            <span class="badge" :class="detail.kind === 'external' ? 'badge-secondary' : 'badge-primary'">
              {{ detail.kind === 'external' ? '对外' : '内接' }}
            </span>
          </div>
        </div>
      </div>
      <div class="modal-actions">
        <button class="btn btn-secondary" type="button" @click="resetSecret">重置 Secret</button>
        <button class="btn btn-primary" type="button" @click="showDetail = false">关闭</button>
      </div>
    </Modal>

    <Modal :open="showSecret" @close="showSecret = false">
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
        <button class="btn btn-primary" type="button" @click="showSecret = false">我已保存</button>
      </div>
    </Modal>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import Modal from '../components/Modal.vue'
import { api } from '../api'
import { store } from '../store'

const scopeCatalog = [
  { name: 'openid', description: '基础身份标识' },
  { name: 'profile:username', description: '用户昵称' },
  { name: 'profile:name', description: '真实姓名' },
  { name: 'profile:avatar', description: '头像' },
  { name: 'profile:student_id', description: '学号' },
  { name: 'email', description: '邮箱地址' },
  { name: 'roles', description: '用户角色列表' }
]

const apps = ref([])
const filter = ref('all')
const canManageApps = computed(() => !!store.user?.is_super_admin)
const pendingCount = computed(() => apps.value.filter((app) => app.status === 'pending').length)
const filteredApps = computed(() => filter.value === 'pending'
  ? apps.value.filter((app) => app.status === 'pending')
  : apps.value)

const loading = ref(true)
const error = ref('')
const submitting = ref(false)
const reviewBusy = ref(false)
const savingM2m = ref(false)
const showCreate = ref(false)
const showDetail = ref(false)
const showSecret = ref(false)
const detail = ref(null)
const detailId = ref('')
const secretInfo = reactive({ title: '', clientId: '', clientSecret: '' })

const form = reactive({
  name: '',
  description: '',
  icon: '',
  redirectUris: '',
  allowedScopes: ['openid'],
  homepageUrl: '',
  isConfidential: true
})

function reviewLabel(status) {
  const labels = { approved: '已通过', pending: '待审核', rejected: '已驳回', suspended: '已暂停' }
  return labels[status] || status || '—'
}
function reviewClass(status) {
  if (status === 'approved') return 'badge-success'
  if (status === 'pending') return 'badge-primary'
  return 'badge-secondary'
}

function resetForm() {
  form.name = ''
  form.description = ''
  form.icon = ''
  form.redirectUris = ''
  form.allowedScopes = ['openid']
  form.homepageUrl = ''
  form.isConfidential = true
}

async function load() {
  loading.value = true
  error.value = ''
  try {
    const appData = await api('/api/apps')
    apps.value = appData?.applications || []
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

function openCreate() {
  resetForm()
  showCreate.value = true
}

async function createApp() {
  error.value = ''
  submitting.value = true
  try {
    const redirects = form.redirectUris.split('\n').map((line) => line.trim()).filter(Boolean)
    const data = await api('/api/apps', {
      method: 'POST',
      body: {
        name: form.name,
        description: form.description,
        icon: form.icon || null,
        redirect_uris: redirects,
        allowed_scopes: form.allowedScopes,
        homepage_url: form.homepageUrl || null,
        is_confidential: form.isConfidential
      }
    })
    showCreate.value = false
    secretInfo.title = '应用创建成功'
    secretInfo.clientId = data.client_id
    secretInfo.clientSecret = data.client_secret || ''
    showSecret.value = true
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    submitting.value = false
  }
}

async function openDetail(app) {
  error.value = ''
  try {
    detail.value = await api('/api/apps/' + app.id)
    detailId.value = app.id
    showDetail.value = true
  } catch (e) {
    error.value = e.message
  }
}

async function review(app, action) {
  let note = ''
  if (action === 'reject') {
    note = prompt('驳回理由：') || ''
    if (!note) return
  }
  if (action === 'suspend' && !confirm(`暂停「${app.name}」？暂停后其 token 接口将直接被拒。`)) return
  error.value = ''
  reviewBusy.value = true
  try {
    await api('/api/apps/' + app.id + '/review', {
      method: 'POST',
      body: { action, note }
    })
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    reviewBusy.value = false
  }
}

async function toggleM2m(event) {
  if (!detailId.value) return
  const enabled = event.target.checked
  savingM2m.value = true
  error.value = ''
  try {
    const data = await api('/api/apps/' + detailId.value, {
      method: 'PUT',
      body: { m2m_enabled: enabled }
    })
    detail.value = data
    const target = apps.value.find((app) => app.id === detailId.value)
    if (target) target.m2m_enabled = data.m2m_enabled
  } catch (e) {
    error.value = e.message
    event.target.checked = !enabled
  } finally {
    savingM2m.value = false
  }
}

async function resetSecret() {
  if (!detailId.value) return
  if (!confirm('确定要重置 Client Secret 吗？旧 secret 将立即失效。')) return
  error.value = ''
  try {
    const data = await api('/api/apps/' + detailId.value + '/reset-secret', { method: 'POST' })
    showDetail.value = false
    secretInfo.title = 'Secret 已重置'
    secretInfo.clientId = detail.value?.client_id || ''
    secretInfo.clientSecret = data.client_secret
    showSecret.value = true
  } catch (e) {
    error.value = e.message
  }
}

async function remove(app) {
  if (!confirm(`确定要删除应用「${app.name}」吗？此操作不可撤销。`)) return
  error.value = ''
  try {
    await api('/api/apps/' + app.id, { method: 'DELETE' })
    await load()
  } catch (e) {
    error.value = e.message
  }
}

onMounted(load)
</script>

<style scoped>
.page-head-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.filter-tabs {
  display: flex;
  gap: 6px;
  margin-bottom: 12px;
}
.filter-tab {
  border: 1px solid var(--line, #e6e6e2);
  background: transparent;
  color: var(--ink-soft, #666);
  font-size: 13px;
  padding: 5px 16px;
  border-radius: 999px;
  cursor: pointer;
}
.filter-tab.is-active {
  border-color: var(--brand, #171717);
  background: var(--brand, #171717);
  color: #fff;
}
</style>
