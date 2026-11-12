import { createRouter, createWebHistory } from 'vue-router'
import { store, loadMe } from './store'

// Route-level code splitting: the phone only ever needs the QR-confirm chunk,
// not the whole app bundle.
const HomeView = () => import('./views/HomeView.vue')
const LoginView = () => import('./views/LoginView.vue')
const RegisterView = () => import('./views/RegisterView.vue')
const VerifyEmailView = () => import('./views/VerifyEmailView.vue')
const ResetPasswordView = () => import('./views/ResetPasswordView.vue')
const QrConfirmView = () => import('./views/QrConfirmView.vue')
const ProfileView = () => import('./views/ProfileView.vue')
const AuthorizeView = () => import('./views/AuthorizeView.vue')
const AdminAppsView = () => import('./views/AdminAppsView.vue')
const AdminHomepageView = () => import('./views/AdminHomepageView.vue')
const AdminUsersView = () => import('./views/AdminUsersView.vue')
const AdminLogsView = () => import('./views/AdminLogsView.vue')
const PrivacyView = () => import('./views/PrivacyView.vue')
const DocsView = () => import('./views/DocsView.vue')
const DeveloperView = () => import('./views/DeveloperView.vue')
const NotFoundView = () => import('./views/NotFoundView.vue')

const routes = [
  // Public landing page — the first thing a logged-out visitor sees, so no
  // redirect to /login and no auth requirement.
  { path: '/', name: 'home', component: HomeView },
  { path: '/login', name: 'login', component: LoginView },
  { path: '/register', name: 'register', component: RegisterView },
  { path: '/verify-email', name: 'verify-email', component: VerifyEmailView },
  { path: '/reset-password', name: 'reset-password', component: ResetPasswordView },
  { path: '/qr/:id', name: 'qr-confirm', component: QrConfirmView, meta: { requiresAuth: true } },
  { path: '/profile', name: 'profile', component: ProfileView, meta: { requiresAuth: true } },
  { path: '/consents', redirect: '/profile' },
  { path: '/authorize', name: 'authorize', component: AuthorizeView, meta: { requiresAuth: true } },
  { path: '/privacy', name: 'privacy', component: PrivacyView },
  // Public developer portal — intentionally no meta: reachable logged out.
  { path: '/docs', name: 'docs', component: DocsView },
  { path: '/developer', name: 'developer', component: DeveloperView },
  { path: '/admin/apps', name: 'admin-apps', component: AdminAppsView, meta: { requiresAuth: true, requiresSuperAdmin: true } },
  { path: '/admin/homepage', name: 'admin-homepage', component: AdminHomepageView, meta: { requiresAuth: true, requiresSuperAdmin: true } },
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
  if (to.meta.requiresSuperAdmin && !store.user?.is_super_admin) {
    return { path: '/profile' }
  }
  if ((to.path === '/login' || to.path === '/register') && store.user) {
    return { path: '/profile' }
  }
  return true
})

export default router
