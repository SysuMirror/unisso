import { reactive } from 'vue'
import { api, resetCsrf } from './api'

export const store = reactive({
  user: null,
  loaded: false
})

export async function loadMe(force = false) {
  if (store.loaded && !force) return store.user
  try {
    const data = await api('/api/auth/session')
    store.user = data?.user || null
  } catch {
    store.user = null
  }
  store.loaded = true
  return store.user
}

export async function signOut() {
  try {
    await api('/api/auth/logout', { method: 'POST' })
  } catch {
    // Session may already be gone; clear local state regardless.
  }
  store.user = null
  store.loaded = true
  resetCsrf()
}
