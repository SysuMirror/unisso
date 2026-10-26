<template>
  <AuthShell kicker="重置密码">
    <form v-if="!success" class="auth-form" @submit.prevent="submit">
      <div v-if="error" class="auth-error">{{ error }}</div>
      <div v-if="notice" class="auth-notice">{{ notice }}</div>

      <p class="reset-copy">
        输入注册邮箱，验证邮箱后设置新密码。重置成功后，其他设备上的登录会全部失效。
      </p>

      <label class="auth-field">
        <span>邮箱</span>
        <input v-model.trim="email" type="email" required autocomplete="email"
               placeholder="you@example.com" autofocus />
      </label>

      <button class="auth-submit auth-submit-secondary" type="button"
              :disabled="sending || cooldown > 0 || !email" @click="sendCode">
        {{ sending ? '发送中…' : cooldown > 0 ? `${cooldown}s 后可重发` : '发送验证码' }}
      </button>

      <label class="auth-field">
        <span>验证码</span>
        <input v-model.trim="code" inputmode="numeric" maxlength="6" required
               placeholder="6 位验证码" autocomplete="one-time-code" />
      </label>

      <label class="auth-field">
        <span>新密码</span>
        <input v-model="password" type="password" required minlength="8"
               autocomplete="new-password" placeholder="至少 8 位，含两类字符" />
      </label>

      <label class="auth-field">
        <span>确认新密码</span>
        <input v-model="confirm" type="password" required minlength="8"
               autocomplete="new-password" placeholder="再次输入新密码" />
      </label>

      <button class="auth-submit" type="submit" :disabled="submitting">
        {{ submitting ? '重置中…' : '重置密码' }}
      </button>
      <div class="reset-links">
        <RouterLink class="auth-alt-link" to="/login">返回登录</RouterLink>
      </div>
    </form>

    <div v-else class="reset-success">
      <h2>密码已重置</h2>
      <p>请使用新密码登录。其他设备上的登录状态已经失效。</p>
      <RouterLink class="auth-submit reset-success-btn" to="/login">返回登录</RouterLink>
    </div>

    <template #note>
      想起密码了？<RouterLink to="/login">直接登录</RouterLink>
    </template>
  </AuthShell>
</template>

<script setup>
import { onUnmounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import AuthShell from '../components/AuthShell.vue'
import { api } from '../api'

const router = useRouter()
const email = ref('')
const code = ref('')
const password = ref('')
const confirm = ref('')
const sending = ref(false)
const submitting = ref(false)
const cooldown = ref(0)
const error = ref('')
const notice = ref('')
const success = ref(false)
let timer = null

function startCooldown(seconds) {
  cooldown.value = seconds
  if (timer) clearInterval(timer)
  timer = setInterval(() => {
    cooldown.value -= 1
    if (cooldown.value <= 0) {
      clearInterval(timer)
      timer = null
    }
  }, 1000)
}

async function sendCode() {
  if (!email.value || sending.value || cooldown.value > 0) return
  error.value = ''
  notice.value = ''
  sending.value = true
  try {
    const data = await api('/api/auth/password-reset/start', {
      method: 'POST',
      body: { email: email.value }
    })
    notice.value = '验证码已发送，请查收邮箱。'
    startCooldown(data?.cooldown || 60)
  } catch (e) {
    error.value = e.message
  } finally {
    sending.value = false
  }
}

async function submit() {
  error.value = ''
  notice.value = ''
  if (password.value !== confirm.value) {
    error.value = '两次输入的新密码不一致'
    return
  }
  submitting.value = true
  try {
    await api('/api/auth/password-reset/confirm', {
      method: 'POST',
      body: {
        email: email.value,
        code: code.value,
        new_password: password.value
      }
    })
    success.value = true
    setTimeout(() => router.push('/login'), 1800)
  } catch (e) {
    error.value = e.message
  } finally {
    submitting.value = false
  }
}

onUnmounted(() => {
  if (timer) clearInterval(timer)
})
</script>

<style scoped>
.reset-copy {
  margin: 0 0 18px;
  font-size: 12.5px;
  line-height: 1.75;
  color: var(--ink-faint);
}
.reset-links {
  display: flex;
  justify-content: flex-end;
  margin-top: 14px;
}
.reset-success {
  padding: 20px 0 8px;
  text-align: center;
}
.reset-success h2 {
  margin: 0 0 10px;
  font-size: 20px;
  letter-spacing: -.02em;
}
.reset-success p {
  margin: 0 0 18px;
  font-size: 12.5px;
  line-height: 1.75;
  color: var(--ink-faint);
}
.reset-success-btn {
  display: inline-flex;
  width: auto;
  padding: 0 26px;
  text-decoration: none;
}
</style>
