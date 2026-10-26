<template>
  <template v-if="!isAuthPage">
    <header class="site-header">
      <div class="header-inner">
        <RouterLink to="/" class="brand">
          <img class="brand-logo" :src="logo" alt="统一身份认证" />
        </RouterLink>

        <button class="nav-toggle" type="button" aria-label="打开菜单"
                :aria-expanded="mobileMenuOpen ? 'true' : 'false'"
                @click="mobileMenuOpen = !mobileMenuOpen">
          <span></span><span></span><span></span>
        </button>

        <nav class="nav" :class="{ 'is-open': mobileMenuOpen }">
          <template v-if="store.user">
            <RouterLink to="/consents" class="nav-link" :class="{ 'is-active': route.path === '/consents' }">我的授权</RouterLink>
            <RouterLink to="/profile" class="nav-link" :class="{ 'is-active': route.path === '/profile' }">个人中心</RouterLink>
            <RouterLink v-if="canManageApps" to="/admin/apps" class="nav-link" :class="{ 'is-active': route.path === '/admin/apps' }">应用</RouterLink>
            <RouterLink v-if="canManageOrgs" to="/admin/orgs" class="nav-link" :class="{ 'is-active': route.path === '/admin/orgs' }">组织</RouterLink>
            <RouterLink v-if="store.user.is_super_admin" to="/admin/users" class="nav-link" :class="{ 'is-active': route.path === '/admin/users' }">用户</RouterLink>
            <RouterLink v-if="store.user.is_super_admin" to="/admin/logs" class="nav-link" :class="{ 'is-active': route.path === '/admin/logs' }">日志</RouterLink>
            <span class="nav-divider" aria-hidden="true"></span>
            <span class="nav-user">
              <span class="user-avatar">
                <img v-if="store.user.avatar" :src="store.user.avatar" alt="" />
                <template v-else>{{ initial }}</template>
              </span>
              <span class="nav-email">{{ displayName }}</span>
            </span>
            <button type="button" class="btn btn-ghost btn-sm" @click="logout">退出</button>
          </template>
          <template v-else>
            <RouterLink to="/login" class="nav-link" :class="{ 'is-active': route.path === '/login' }">登录</RouterLink>
            <RouterLink to="/register" class="btn btn-primary btn-sm">注册</RouterLink>
          </template>
        </nav>
      </div>
    </header>

    <img class="app-watermark" :src="logo" alt="" aria-hidden="true" />

    <main class="container">
      <RouterView />
    </main>

    <DeviceApprovalPrompt />

    <footer class="site-footer">
      <div class="footer-inner">
        <span>统一身份认证</span>
        <span class="footer-meta">
          <RouterLink to="/privacy">隐私政策</RouterLink>
          <span class="footer-dot">·</span>
          安全登录
        </span>
      </div>
    </footer>
  </template>

  <RouterView v-else />
</template>

<script setup>
import { computed, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { store, signOut } from './store'
import DeviceApprovalPrompt from './components/DeviceApprovalPrompt.vue'
import logo from './assets/logo.png'

const route = useRoute()
const router = useRouter()
const mobileMenuOpen = ref(false)

watch(() => route.fullPath, () => { mobileMenuOpen.value = false })

const isAuthPage = computed(() =>
  ['/login', '/register', '/verify-email', '/reset-password', '/step-up', '/authorize'].includes(route.path) ||
  route.path.startsWith('/qr/'))

const displayName = computed(() => {
  const user = store.user
  if (!user) return ''
  return user.full_name || user.username || user.email?.split('@')[0] || ''
})

const initial = computed(() => (displayName.value || '?').slice(0, 1).toUpperCase())

const orgRoles = computed(() => {
  const user = store.user
  return Array.isArray(user?.organizations)
    ? user.organizations.map((org) => org.role)
    : []
})

const canManageApps = computed(() => {
  const user = store.user
  if (!user) return false
  return user.is_admin || user.is_super_admin ||
    orgRoles.value.some((role) => role === 'admin' || role === 'developer')
})

const canManageOrgs = computed(() => {
  const user = store.user
  if (!user) return false
  return user.is_super_admin || user.is_admin || orgRoles.value.includes('admin')
})

async function logout() {
  mobileMenuOpen.value = false
  await signOut()
  router.push('/login')
}
</script>
