import { createRouter, createWebHistory } from 'vue-router'
import { store, loadMe } from './store'

// Route-level code splitting: the phone only ever needs the QR-confirm chunk,
// not the whole app bundle.
const LoginView = () => import('./views/LoginView.vue')
const RegisterView = () => import('./views/RegisterView.vue')
const VerifyEmailView = () => import('./views/VerifyEmailView.vue')
const ResetPasswordView = () => import('./views/ResetPasswordView.vue')
const QrConfirmView = () => import('./views/QrConfirmView.vue')
const StepUpView = () => import('./views/StepUpView.vue')
const DashboardView = () => import('./views/DashboardView.vue')
const ProfileView = () => import('./views/ProfileView.vue')
const ConsentsView = () => import('./views/ConsentsView.vue')
const AuthorizeView = () => import('./views/AuthorizeView.vue')
const AdminAppsView = () => import('./views/AdminAppsView.vue')
const AdminOrgsView = () => import('./views/AdminOrgsView.vue')
const AdminUsersView = () => import('./views/AdminUsersView.vue')
const AdminLogsView = () => import('./views/AdminLogsView.vue')
const PrivacyView = () => import('./views/PrivacyView.vue')
const NotFoundView = () => import('./views/NotFoundView.vue')

const routes = [
  { path: '/', name: 'dashboard', component: DashboardView, meta: { requiresAuth: true } },
  { path: '/login', name: 'login', component: LoginView },
  { path: '/register', name: 'register', component: RegisterView },
  { path: '/verify-email', name: 'verify-email', component: VerifyEmailView },
  { path: '/reset-password', name: 'reset-password', component: ResetPasswordView },
  { path: '/step-up', name: 'step-up', component: StepUpView },
  { path: '/qr/:id', name: 'qr-confirm', component: QrConfirmView, meta: { requiresAuth: true } },
  { path: '/profile', name: 'profile', component: ProfileView, meta: { requiresAuth: true } },
  { path: '/consents', name: 'consents', component: ConsentsView, meta: { requiresAuth: true } },
  { path: '/authorize', name: 'authorize', component: AuthorizeView, meta: { requiresAuth: true } },
  { path: '/privacy', name: 'privacy', component: PrivacyView },
  { path: '/admin/apps', name: 'admin-apps', component: AdminAppsView, meta: { requiresAuth: true } },
  { path: '/admin/orgs', name: 'admin-orgs', component: AdminOrgsView, meta: { requiresAuth: true } },
  { path: '/admin/org', redirect: '/admin/orgs' },
  { path: '/admin/users', name: 'admin-users', component: AdminUsersView, meta: { requiresAuth: true, requiresSuperAdmin: true } },
  { path: '/admin/logs', name: 'admin-logs', component: AdminLogsView, meta: { requiresAuth: true, requiresSuperAdmin: true } },
  { path: '/:pathMatch(.*)*', name: 'not-found', component: NotFoundView }
]

const router = createRouter({
  history: createWebHistory(),
  routes,
  scrollBehavior() {
    return { top: 0 }
  }
})

router.beforeEach(async (to) => {
  await loadMe()

  if (to.meta.requiresAuth && !store.user) {
    return { path: '/login', query: { next: to.fullPath } }
  }
  if (to.meta.requiresAdmin && !store.user?.is_admin) {
    return { path: '/' }
  }
  if (to.meta.requiresSuperAdmin && !store.user?.is_super_admin) {
    return { path: '/' }
  }
  if ((to.path === '/login' || to.path === '/register') && store.user) {
    return { path: '/' }
  }
  return true
})

export default router
