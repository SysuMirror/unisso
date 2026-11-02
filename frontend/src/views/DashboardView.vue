<template>
  <div v-if="user">
    <div class="page-head dashboard-head">
      <div>
        <h1 class="page-title">账户概览</h1>
        <p class="page-sub">管理你的身份、授权与接入应用。</p>
      </div>
    </div>

    <RouterLink v-if="user.step_up_enabled === false" to="/profile" class="face-tip">
      建议开启「登录保护」：新设备或长期未登录时增加一次二次验证，账号被盗风险会明显降低。
      <span>去开启 →</span>
    </RouterLink>
    <RouterLink v-else-if="showFaceTip" to="/profile" class="face-tip">
      建议登记人脸：新设备或长期未登录时可以作为二次验证，降低账号丢失风险。
      <span>去登记 →</span>
    </RouterLink>

    <section class="dashboard-identity">
      <div class="identity-left">
        <span class="avatar avatar-xl">
          <img v-if="user.avatar" :src="user.avatar" alt="" />
          <template v-else>{{ initial }}</template>
        </span>
        <div>
          <div class="identity-name">{{ displayName }}</div>
          <div class="identity-mail">{{ user.email }}</div>
        </div>
      </div>
      <RouterLink to="/profile" class="identity-edit">编辑资料 →</RouterLink>
    </section>

    <section class="meta-strip">
      <div class="meta-cell">
        <span class="meta-label">身份</span>
        <span class="meta-value">
          <span class="badge" :class="user.is_internal ? 'badge-primary' : 'badge-secondary'">
            {{ user.is_internal ? '院内' : '院外' }}
          </span>
        </span>
      </div>
      <div class="meta-cell">
        <span class="meta-label">账号关联</span>
        <span class="meta-value">{{ user.linked_accounts?.length ? '已关联 ' + user.linked_accounts.length + ' 个' : '独立账号' }}</span>
      </div>
      <div class="meta-cell">
        <span class="meta-label">邮箱</span>
        <span class="meta-value">{{ user.email_verified ? '已验证' : '未验证' }}</span>
      </div>
      <div class="meta-cell">
        <span class="meta-label">绑定账号</span>
        <span class="meta-value">{{ user.identities?.length || 0 }} 个</span>
      </div>
      <div class="meta-cell">
        <span class="meta-label">最近登录</span>
        <span class="meta-value">{{ formatDateTime(user.last_login_at) }}</span>
      </div>
    </section>

    <nav class="quick-inline">
      <RouterLink to="/profile">个人中心 <span>→</span></RouterLink>
      <RouterLink to="/consents">我的授权 <span>→</span></RouterLink>
      <RouterLink v-if="user.is_admin" to="/admin/apps">应用管理 <span>→</span></RouterLink>
      <RouterLink v-if="user.is_admin" to="/admin/users">用户管理 <span>→</span></RouterLink>
    </nav>

    <div class="dashboard-panels">
      <section class="dashboard-panel">
        <div class="section-head">
          <h2 class="section-title">最近授权</h2>
          <RouterLink to="/consents" class="section-link">查看全部 →</RouterLink>
        </div>
        <div v-if="consents.length" class="stack-list">
          <div v-for="consent in consents" :key="consent.id" class="stack-row">
            <img v-if="consent.application.icon" :src="consent.application.icon" class="stack-icon" alt="" />
            <span v-else class="stack-icon">{{ (consent.application.name || '?').slice(0, 1) }}</span>
            <div class="stack-main">
              <div class="stack-title">{{ consent.application.name }}</div>
              <div class="stack-sub">{{ scopeSummary(consent.scopes) }}</div>
            </div>
            <div class="stack-date">{{ formatDate(consent.granted_at) }}</div>
          </div>
        </div>
        <div v-else-if="!consentsLoading" class="compact-empty">暂无授权记录</div>

        <div class="panel-divider"></div>

        <div class="section-head">
          <h2 class="section-title">登录方式</h2>
        </div>
        <div class="status-list">
          <div class="status-row"><span>邮箱密码</span><span>已启用</span></div>
          <div class="status-row"><span>集市扫码</span><span>已启用</span></div>
          <div class="status-row"><span>集市账号</span><span>{{ sseBound ? '已绑定' : '未绑定' }}</span></div>
        </div>
      </section>

      <section class="dashboard-panel">
        <div class="section-head">
          <h2 class="section-title">已绑定账号</h2>
          <RouterLink to="/profile" class="section-link">管理 →</RouterLink>
        </div>
        <div v-if="user.identities?.length" class="stack-list">
          <div v-for="identity in user.identities" :key="identity.id" class="stack-row">
            <span class="stack-provider">{{ providerLabel(identity.provider) }}</span>
            <div class="stack-main">
              <div class="stack-title">{{ identity.provider_username || identity.provider_user_id }}</div>
              <div v-if="identity.provider_email" class="stack-sub">{{ identity.provider_email }}</div>
            </div>
            <div class="stack-date">{{ formatDate(identity.created_at) }}</div>
          </div>
        </div>
        <div v-else class="compact-empty">
          暂无绑定账号 <RouterLink to="/profile" class="inline-link">去绑定 →</RouterLink>
        </div>
      </section>
    </div>

    <p class="dashboard-footnote">
      <span>建议定期检查授权记录，并绑定备用账号以便找回。</span>
      <RouterLink to="/profile">去设置 →</RouterLink>
    </p>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { store } from '../store'
import { api } from '../api'

const user = computed(() => store.user)
const consents = ref([])
const consentsLoading = ref(true)
const showFaceTip = ref(false)

const scopeLabels = {
  openid: '基础身份',
  'profile:username': '昵称',
  'profile:name': '姓名',
  'profile:avatar': '头像',
  'profile:student_id': '学号',
  email: '邮箱',
  roles: '角色'
}

const displayName = computed(() => {
  const u = store.user
  if (!u) return ''
  return u.full_name || u.username || u.email?.split('@')[0] || ''
})
const initial = computed(() => (displayName.value || '?').slice(0, 1).toUpperCase())

const sseBound = computed(() => {
  const u = store.user
  if (!u) return false
  return !!u.is_internal || (u.identities || []).some((identity) => ['sse', 'ssemarket'].includes(identity.provider))
})


function providerLabel(provider) {
  const labels = { sse: '集市账号', ssemarket: '集市账号', github: 'GitHub', wechat: '微信', custom: '其他' }
  return labels[provider] || provider
}

function scopeSummary(scopes) {
  if (!scopes?.length) return '—'
  return scopes.map((scope) => scopeLabels[scope] || '其他信息').join(' · ')
}

function formatDate(value) {
  if (!value) return ''
  return String(value).replace('T', ' ').slice(0, 10)
}

function formatDateTime(value) {
  if (!value) return '—'
  return String(value).replace('T', ' ').slice(0, 16)
}

onMounted(async () => {
  try {
    const data = await api('/api/consents')
    consents.value = (data?.consents || []).slice(0, 3)
  } catch {
    consents.value = []
  } finally {
    consentsLoading.value = false
  }
  try {
    const status = await api('/api/account/faces/status')
    showFaceTip.value = !!status.enabled && !status.face_count
  } catch {
    showFaceTip.value = false
  }
})
</script>

<style scoped>
.face-tip {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 12px;
  margin: 0 0 18px;
  padding: 11px 14px;
  border-left: 2px solid var(--line-strong);
  font-size: 12.5px;
  line-height: 1.7;
  color: var(--ink-soft);
}
.face-tip span { color: var(--ink); white-space: nowrap; }
</style>
