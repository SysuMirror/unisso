<template>
  <div>
    <div class="page-head">
      <div>
        <h1 class="page-title">组织与应用权限</h1>
        <p class="page-sub">
          组织只用于管理应用归属与开发者，不进入 OAuth 身份 claims，也不是业务用户组。
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
          <span class="org-chip-meta">{{ roleLabel(org.role) }}</span>
        </button>
      </div>

      <div class="panel">
        <div class="panel-head">
          <div>
            <h2 class="panel-title">{{ currentOrg?.name }}</h2>
            <p class="panel-sub">{{ members.length }} 位成员 · {{ currentOrg?.slug }}</p>
          </div>
          <span class="badge" :class="canAdmin ? 'badge-admin' : 'badge-secondary'">
            {{ canAdmin ? '可管理' : '仅成员' }}
          </span>
        </div>

        <div v-if="!canAdmin" class="compact-empty">
          你不是该组织的管理员，只能查看；请联系组织管理员或超级管理员。
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
            组织管理员可以设置其他管理员和开发者；开发者可以注册/管理本组织应用。
            组织不会改变用户在 OAuth 中返回的身份或权限。
          </p>
        </template>
      </div>
    </template>

    <div v-else-if="!loading" class="compact-empty">你还没有加入任何组织。</div>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { api } from '../api'
import { store } from '../store'

const orgs = ref([])
const members = ref([])
const selectedId = ref('')
const isSuper = ref(false)
const loading = ref(true)
const busy = ref(false)
const error = ref('')
const notice = ref('')
const showCreate = ref(false)
const newOrg = reactive({ name: '', slug: '' })
const newMember = reactive({ email: '', role: 'member' })

const me = computed(() => store.user)
const currentOrg = computed(() => orgs.value.find((o) => o.id === selectedId.value))
const canAdmin = computed(() => !!currentOrg.value?.can_admin)

const roleLabels = { admin: '管理员', developer: '开发者', member: '成员' }
const roleLabel = (role) => roleLabels[role] || role

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

async function selectOrg(org) {
  if (org.id === selectedId.value) return
  selectedId.value = org.id
  error.value = ''
  notice.value = ''
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
  .form-row, .member-add { grid-template-columns: 1fr; }
  .form-group-action { display: block; }
  .role-select { width: 100%; }
  .footnote { margin: 14px 16px 16px; }
}
</style>
