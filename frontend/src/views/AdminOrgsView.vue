<template>
  <div>
    <div class="page-head">
      <div>
        <h1 class="page-title">组织</h1>
        <p class="page-sub">
          加入组织后即可在该组织下创建应用。可申请加入，由组织管理员或超级管理员审批。
        </p>
      </div>
      <button v-if="isSuper" class="btn btn-secondary" type="button" @click="showCreate = !showCreate">
        {{ showCreate ? '收起' : '新建组织' }}
      </button>
    </div>

    <div v-if="error" class="alert alert-danger">{{ error }}</div>
    <div v-if="notice" class="alert alert-success">{{ notice }}</div>

    <form v-if="showCreate" class="panel org-create" @submit.prevent="createOrg">
      <div class="form-row">
        <div class="form-group">
          <label class="form-label">组织名称 *</label>
          <input v-model.trim="newOrg.name" class="form-input" required placeholder="例如：计算机学院" />
        </div>
        <div class="form-group">
          <label class="form-label">组织标识</label>
          <input v-model.trim="newOrg.slug" class="form-input" placeholder="留空自动生成，如 cs" />
        </div>
        <div class="form-group form-group-action">
          <button class="btn btn-primary" type="submit" :disabled="busy">创建</button>
        </div>
      </div>
    </form>

    <div v-if="loading" class="loading-line"><span class="spinner"></span>加载中…</div>

    <template v-else-if="orgs.length">
      <div class="org-switch">
        <button
          v-for="org in orgs"
          :key="org.id"
          type="button"
          class="org-chip"
          :class="{ 'is-active': org.id === selectedId }"
          @click="selectOrg(org)"
        >
          <span>{{ org.name }}</span>
          <span class="org-chip-meta">{{ chipMeta(org) }}</span>
        </button>
      </div>

      <div v-if="reviewRequests.length" class="panel join-requests">
        <div class="panel-head">
          <div>
            <h2 class="panel-title">待审批的加入申请</h2>
            <p class="panel-sub">通过后申请人将成为组织成员，即可创建应用</p>
          </div>
          <span class="badge badge-secondary">{{ reviewRequests.length }} 条</span>
        </div>
        <div class="table-wrap">
          <table class="table">
            <thead>
              <tr><th>申请人</th><th>邮箱</th><th>组织</th><th>理由</th><th>时间</th><th>操作</th></tr>
            </thead>
            <tbody>
              <tr v-for="r in reviewRequests" :key="r.id">
                <td>
                  <div class="cell-ident">
                    <span class="user-avatar">{{ (r.user.full_name || r.user.username || r.user.email || '?').slice(0, 1).toUpperCase() }}</span>
                    <div class="cell-ident-text">
                      <div class="cell-main">{{ r.user.username || r.user.email }}</div>
                      <div v-if="r.user.full_name" class="cell-sub">{{ r.user.full_name }}</div>
                    </div>
                  </div>
                </td>
                <td>{{ r.user.email || '-' }}</td>
                <td>{{ r.organization.name }}</td>
                <td class="text-secondary request-message">{{ r.message || '—' }}</td>
                <td class="text-secondary">{{ formatDate(r.created_at) }}</td>
                <td class="review-actions">
                  <button class="btn btn-primary btn-sm" type="button" :disabled="busy"
                          @click="reviewRequest(r, 'approve')">通过</button>
                  <button class="btn btn-danger btn-sm" type="button" :disabled="busy"
                          @click="reviewRequest(r, 'reject')">拒绝</button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>

      <div v-if="currentOrg" class="panel">
        <div class="panel-head">
          <div>
            <h2 class="panel-title">{{ currentOrg.name }}</h2>
            <p class="panel-sub">{{ currentOrg.member_count }} 位成员 · {{ currentOrg.slug }}</p>
          </div>
          <span class="badge" :class="canAdmin ? 'badge-admin' : 'badge-secondary'">{{ badgeText }}</span>
        </div>

        <div v-if="!currentOrg.joined" class="join-apply">
          <p v-if="currentOrg.description" class="join-desc">{{ currentOrg.description }}</p>
          <div v-if="currentOrg.request_status === 'pending'" class="join-pending">
            申请已提交（{{ formatDate(myRequest?.created_at) }}），正在等待管理员审批。
          </div>
          <template v-else>
            <div v-if="currentOrg.request_status === 'rejected'" class="join-rejected">
              上次申请被拒绝<span v-if="myRequest?.review_note">：{{ myRequest.review_note }}</span>，欢迎补充理由后再次申请。
            </div>
            <form class="join-form" @submit.prevent="applyJoin">
              <input v-model.trim="applyMessage" class="form-input" maxlength="160"
                     placeholder="申请理由（可选，方便管理员了解你）" />
              <button class="btn btn-primary" type="submit" :disabled="busy">申请加入</button>
            </form>
          </template>
        </div>

        <div v-else-if="!canAdmin" class="compact-empty">
          你是该组织的{{ roleLabel(currentOrg.role) }}，可以在「应用」页面创建和管理本组织的应用。
        </div>

        <template v-else>
          <form class="member-add" @submit.prevent="addMember">
            <input v-model.trim="newMember.email" class="form-input" type="email" required
                   placeholder="成员邮箱（需已注册）" />
            <select v-model="newMember.role" class="form-input form-select">
              <option value="member">成员</option>
              <option value="developer">开发者</option>
              <option value="admin">管理员</option>
            </select>
            <button class="btn btn-primary" type="submit" :disabled="busy">添加</button>
          </form>

          <div class="table-wrap">
            <table class="table">
              <thead>
                <tr><th>成员</th><th>邮箱</th><th>角色</th><th>加入时间</th><th>操作</th></tr>
              </thead>
              <tbody>
                <tr v-for="member in members" :key="member.id">
                  <td>
                    <div class="cell-ident">
                      <span class="user-avatar">{{ (member.full_name || member.username || member.email || '?').slice(0, 1).toUpperCase() }}</span>
                      <div class="cell-ident-text">
                        <div class="cell-main">{{ member.username || member.email }}</div>
                        <div v-if="member.full_name" class="cell-sub">{{ member.full_name }}</div>
                      </div>
                    </div>
                  </td>
                  <td>{{ member.email || '-' }}</td>
                  <td>
                    <select
                      class="form-input form-select role-select"
                      :value="member.role"
                      :disabled="busy"
                      @change="setRole(member, $event.target.value)"
                    >
                      <option value="member">成员</option>
                      <option value="developer">开发者</option>
                      <option value="admin">管理员</option>
                    </select>
                  </td>
                  <td class="text-secondary">{{ member.joined_at ? formatDate(member.joined_at) : '-' }}</td>
                  <td>
                    <button v-if="member.id !== me?.id" class="btn btn-danger btn-sm" type="button"
                            :disabled="busy" @click="removeMember(member)">移除</button>
                    <span v-else class="text-xs text-muted">当前账号</span>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>

          <p class="footnote">
            组织管理员可以设置其他管理员和开发者，并审批加入申请；组织成员都可以注册/管理本组织的应用。
            组织不会改变用户在 OAuth 中返回的身份或权限。
          </p>
        </template>
      </div>
    </template>

    <div v-else-if="!loading" class="compact-empty">还没有任何组织，请联系超级管理员创建。</div>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { api } from '../api'
import { store } from '../store'

const orgs = ref([])
const members = ref([])
const reviewRequests = ref([])
const myRequests = ref([])
const selectedId = ref('')
const isSuper = ref(false)
const loading = ref(true)
const busy = ref(false)
const error = ref('')
const notice = ref('')
const showCreate = ref(false)
const newOrg = reactive({ name: '', slug: '' })
const newMember = reactive({ email: '', role: 'member' })
const applyMessage = ref('')

const me = computed(() => store.user)
const currentOrg = computed(() => orgs.value.find((o) => o.id === selectedId.value))
const canAdmin = computed(() => !!currentOrg.value?.can_admin)
const myRequest = computed(() =>
  myRequests.value.find((r) => r.organization?.id === selectedId.value))
const badgeText = computed(() => {
  const org = currentOrg.value
  if (!org) return ''
  if (!org.joined) return '未加入'
  if (canAdmin.value) return '可管理'
  return roleLabel(org.role)
})

const roleLabels = { admin: '管理员', developer: '开发者', member: '成员' }
const roleLabel = (role) => roleLabels[role] || role

function chipMeta(org) {
  if (org.joined) return roleLabel(org.role)
  return org.request_status === 'pending' ? '待审批' : '未加入'
}

function formatDate(value) {
  if (!value) return ''
  return String(value).replace('T', ' ').slice(0, 10)
}

async function loadOrgs() {
  const data = await api('/api/orgs')
  orgs.value = data.organizations || []
  isSuper.value = !!data.is_super_admin
  if (!selectedId.value || !orgs.value.some((o) => o.id === selectedId.value)) {
    selectedId.value = orgs.value[0]?.id || ''
  }
  const current = orgs.value.find((o) => o.id === selectedId.value)
  if (selectedId.value && (isSuper.value || current?.can_admin)) {
    await loadMembers()
  } else {
    members.value = []
  }
}

async function loadMembers() {
  if (!selectedId.value) return
  const data = await api(`/api/orgs/${selectedId.value}/members`)
  members.value = data.members || []
}

async function loadReviewRequests() {
  try {
    const data = await api('/api/org-join-requests')
    reviewRequests.value = data.requests || []
  } catch {
    reviewRequests.value = []
  }
}

async function loadMine() {
  try {
    const data = await api('/api/org-join-requests/mine')
    myRequests.value = data.requests || []
  } catch {
    myRequests.value = []
  }
}

async function selectOrg(org) {
  if (org.id === selectedId.value) return
  selectedId.value = org.id
  error.value = ''
  notice.value = ''
  applyMessage.value = ''
  loading.value = true
  try {
    if (isSuper.value || org.can_admin) await loadMembers()
    else members.value = []
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

async function applyJoin() {
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    await api(`/api/orgs/${selectedId.value}/join-requests`, {
      method: 'POST',
      body: { message: applyMessage.value }
    })
    applyMessage.value = ''
    notice.value = '申请已提交，等待管理员审批。'
    await Promise.all([loadOrgs(), loadMine()])
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function reviewRequest(r, action) {
  if (action === 'reject' &&
      !window.confirm(`拒绝 ${r.user.full_name || r.user.email} 加入「${r.organization.name}」的申请？`)) return
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    await api(`/api/org-join-requests/${r.id}/review`, {
      method: 'POST',
      body: { action }
    })
    notice.value = action === 'approve'
      ? `已通过申请，${r.user.email} 现在是「${r.organization.name}」的成员。`
      : '已拒绝该申请。'
    await Promise.all([loadOrgs(), loadReviewRequests(), loadMine()])
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function addMember() {
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    await api(`/api/orgs/${selectedId.value}/members`, {
      method: 'POST',
      body: { email: newMember.email, role: newMember.role }
    })
    newMember.email = ''
    newMember.role = 'member'
    notice.value = '成员已添加或角色已更新。'
    await loadOrgs()
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function setRole(member, role) {
  if (role === member.role) return
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    await api(`/api/orgs/${selectedId.value}/members/${member.id}`, {
      method: 'PUT',
      body: { role }
    })
    notice.value = '角色已更新。'
    await loadMembers()
  } catch (e) {
    error.value = e.message
    await loadMembers()
  } finally {
    busy.value = false
  }
}

async function removeMember(member) {
  if (!window.confirm(`移除 ${member.full_name || member.email}？`)) return
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    await api(`/api/orgs/${selectedId.value}/members/${member.id}`, { method: 'DELETE' })
    notice.value = '成员已移除。'
    await loadOrgs()
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function createOrg() {
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    const data = await api('/api/orgs', { method: 'POST', body: { ...newOrg } })
    newOrg.name = ''
    newOrg.slug = ''
    showCreate.value = false
    notice.value = '组织已创建，你是该组织管理员。'
    selectedId.value = data.organization?.id || selectedId.value
    await loadOrgs()
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

onMounted(async () => {
  try {
    await loadOrgs()
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
  await Promise.all([loadReviewRequests(), loadMine()])
})
</script>

<style scoped>
.org-create { margin-bottom: 18px; }
.form-row {
  display: grid;
  grid-template-columns: minmax(0, 2fr) minmax(0, 1fr) auto;
  gap: 16px;
  align-items: end;
}
.form-group-action { display: flex; align-items: flex-end; }
.org-switch {
  display: flex;
  flex-wrap: nowrap;
  gap: 26px;
  margin-bottom: 20px;
  border-bottom: 1px solid var(--line);
  overflow-x: auto;
  scrollbar-width: thin;
}
.org-chip {
  position: relative;
  display: inline-flex;
  align-items: baseline;
  gap: 8px;
  padding: 10px 0 11px;
  border: 0;
  background: transparent;
  color: var(--ink-soft);
  font: inherit;
  font-size: 13.5px;
  white-space: nowrap;
  cursor: pointer;
}
.org-chip:hover { color: var(--ink); }
.org-chip.is-active { color: var(--ink); font-weight: 600; }
.org-chip.is-active::after {
  content: "";
  position: absolute;
  left: 0;
  right: 0;
  bottom: -1px;
  height: 2px;
  background: var(--ink);
}
.org-chip-meta { font-size: 11px; font-weight: 400; color: var(--ink-faint); }
.panel-head {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 16px;
  padding: 16px 18px;
  border-bottom: 1px solid var(--line);
}
.panel-title { margin: 0; font-size: 15px; font-weight: 600; color: var(--ink); }
.panel-sub { margin: 3px 0 0; font-size: 12px; color: var(--ink-faint); }
.join-requests { margin-bottom: 20px; }
.request-message { max-width: 220px; overflow-wrap: anywhere; }
.review-actions { display: flex; gap: 8px; white-space: nowrap; }
.join-apply { padding: 18px; }
.join-desc { margin: 0 0 14px; font-size: 13px; color: var(--ink-soft); }
.join-pending {
  padding: 14px 16px;
  border: 1px solid var(--line);
  border-radius: 10px;
  background: var(--bg-soft, rgba(0, 0, 0, 0.02));
  font-size: 13px;
  color: var(--ink-soft);
}
.join-rejected {
  margin-bottom: 12px;
  padding: 12px 14px;
  border: 1px solid rgba(180, 60, 50, 0.35);
  border-radius: 10px;
  background: rgba(180, 60, 50, 0.06);
  font-size: 13px;
  color: var(--ink-soft);
}
.join-form { display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: 10px; }
.member-add {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 140px auto;
  gap: 10px;
  padding: 14px 18px;
  border-bottom: 1px solid var(--line);
}
.form-select {
  appearance: none;
  background-image: linear-gradient(45deg, transparent 50%, var(--ink-faint) 50%),
    linear-gradient(135deg, var(--ink-faint) 50%, transparent 50%);
  background-position: calc(100% - 15px) 50%, calc(100% - 10px) 50%;
  background-size: 5px 5px, 5px 5px;
  background-repeat: no-repeat;
  padding-right: 30px;
}
.role-select { width: 130px; padding-top: 4px; padding-bottom: 4px; }
.footnote { margin: 14px 18px 18px; font-size: 12px; line-height: 1.7; color: var(--ink-faint); }
@media (max-width: 720px) {
  .form-row, .member-add, .join-form { grid-template-columns: 1fr; }
  .form-group-action { display: block; }
  .role-select { width: 100%; }
  .footnote { margin: 14px 16px 16px; }
}
</style>
