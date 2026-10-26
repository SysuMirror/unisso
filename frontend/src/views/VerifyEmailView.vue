<template>
  <AuthShell kicker="验证邮箱">
    <form class="auth-form" @submit.prevent="verify">
      <div v-if="error" class="auth-error">{{ error }}</div>
      <div v-if="notice" class="auth-error" style="border-left-color: var(--ok); color: var(--ok);">{{ notice }}</div>

      <label class="auth-field">
        <span>邮箱</span>
        <input id="verify-email" v-model.trim="email" type="email" required maxlength="128" autocomplete="email" />
      </label>

      <label class="auth-field">
        <span>验证码</span>
        <input id="verify-code" v-model.trim="code" type="text" required inputmode="numeric"
               pattern="[0-9]{6}" maxlength="6" autocomplete="one-time-code" placeholder="6 位数字" />
      </label>

      <button class="auth-submit" type="submit" :disabled="loading">
        {{ loading ? '验证中…' : '验证并登录' }}
      </button>
    </form>

    <template #note>
      <button type="button" :disabled="resending" @click="resend">
        {{ resending ? '发送中…' : '重新发送' }}
      </button>
      <span class="auth-sep">·</span>
      <RouterLink to="/login">返回登录</RouterLink>
    </template>
  </AuthShell>
</template>

<script setup>
import { ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AuthShell from '../components/AuthShell.vue'
import { api, resetCsrf } from '../api'
import { store } from '../store'

const route = useRoute()
const router = useRouter()
const email = ref(typeof route.query.email === 'string' ? route.query.email : '')
const code = ref('')
const loading = ref(false)
const resending = ref(false)
const error = ref('')
const notice = ref('')

async function verify() {
  error.value = ''
  notice.value = ''
  loading.value = true
  try {
    const data = await api('/api/auth/verify-email', {
      method: 'POST',
      body: { email: email.value, code: code.value }
    })
    resetCsrf()
    store.user = data.user
    store.loaded = true
    router.push('/')
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

async function resend() {
  error.value = ''
  notice.value = ''
  resending.value = true
  try {
    await api('/api/auth/resend-registration', {
      method: 'POST',
      body: { email: email.value }
    })
    notice.value = '新的验证码已发送'
  } catch (e) {
    error.value = e.message
  } finally {
    resending.value = false
  }
}
</script>
