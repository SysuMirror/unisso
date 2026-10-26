<template>
  <AuthShell kicker="创建账号">
    <form class="auth-form" @submit.prevent="submit">
      <div v-if="error" class="auth-error">{{ error }}</div>

      <label class="auth-field">
        <span>邮箱</span>
        <input id="register-email" v-model.trim="email" type="email" required maxlength="128"
               autocomplete="email" placeholder="you@example.com" autofocus />
      </label>

      <label class="auth-field">
        <span>密码</span>
        <input id="register-password" v-model="password" type="password" required minlength="8"
               maxlength="128" autocomplete="new-password" placeholder="至少 8 位" />
        <em class="auth-hint">大小写、数字、符号，至少两类</em>
      </label>

      <div class="auth-row">
        <label class="auth-field">
          <span>昵称</span>
          <input id="register-username" v-model.trim="username" type="text" maxlength="64" placeholder="可选" />
        </label>
        <label class="auth-field">
          <span>姓名</span>
          <input id="register-name" v-model.trim="fullName" type="text" maxlength="128" placeholder="可选" />
        </label>
      </div>

      <label class="auth-field">
        <span>邀请码 <em class="auth-hint-inline">可选</em></span>
        <input id="register-invite" v-model.trim="inviteCode" type="text" maxlength="64"
               placeholder="填写后同步注册集市账号" />
      </label>

      <button class="auth-submit" type="submit" :disabled="loading">
        {{ loading ? '发送验证码…' : '创建账号' }}
      </button>
    </form>

    <template #note>
      已有账号？<RouterLink to="/login">登录</RouterLink>
    </template>
  </AuthShell>

  <Modal :open="sseModalOpen" @close="sseModalOpen = false">
    <template #title>验证邮箱</template>
    <p class="page-sub">验证码已发送到 {{ email }}；集市验证码也已发送。</p>
    <div v-if="modalError" class="auth-error">{{ modalError }}</div>
    <form @submit.prevent="finishInvite">
      <div class="form-group">
        <label class="form-label">UniSSO 验证码</label>
        <input v-model.trim="unissocode" class="form-input" inputmode="numeric"
               pattern="[0-9]{6}" maxlength="6" required placeholder="6 位数字" />
      </div>
      <div class="form-group">
        <label class="form-label">集市验证码</label>
        <input v-model.trim="sseCode" class="form-input" inputmode="numeric"
               pattern="[0-9]{6}" maxlength="6" required placeholder="6 位数字" />
      </div>
      <div class="modal-actions">
        <button class="btn btn-secondary" type="button" @click="sseModalOpen = false">取消</button>
        <button class="btn btn-primary" type="submit" :disabled="modalLoading">
          {{ modalLoading ? '提交中…' : '完成注册' }}
        </button>
      </div>
    </form>
  </Modal>
</template>

<script setup>
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import AuthShell from '../components/AuthShell.vue'
import Modal from '../components/Modal.vue'
import { api, resetCsrf } from '../api'
import { store } from '../store'

const router = useRouter()
const email = ref('')
const password = ref('')
const username = ref('')
const fullName = ref('')
const inviteCode = ref('')
const loading = ref(false)
const error = ref('')

const sseModalOpen = ref(false)
const modalLoading = ref(false)
const modalError = ref('')
const unissocode = ref('')
const sseCode = ref('')

async function submit() {
  error.value = ''
  loading.value = true
  try {
    const data = await api('/api/auth/register', {
      method: 'POST',
      body: {
        email: email.value,
        password: password.value,
        username: username.value,
        full_name: fullName.value,
        invite_code: inviteCode.value
      }
    })
    if (data?.sse_code_required) {
      sseModalOpen.value = true
      return
    }
    router.push({ path: '/verify-email', query: { email: email.value } })
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

async function finishInvite() {
  modalError.value = ''
  modalLoading.value = true
  try {
    const data = await api('/api/auth/verify-email', {
      method: 'POST',
      body: {
        email: email.value,
        code: unissocode.value,
        sse_code: sseCode.value,
        password: password.value,
        invite_code: inviteCode.value
      }
    })
    resetCsrf()
    store.user = data.user
    store.loaded = true
    sseModalOpen.value = false
    router.push('/')
  } catch (e) {
    modalError.value = e.message
  } finally {
    modalLoading.value = false
  }
}
</script>
