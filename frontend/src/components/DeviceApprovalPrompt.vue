<template>
  <Modal :open="!!current" @close="snooze">
    <template #title>登录确认</template>
    <div v-if="current" class="approval">
      <p class="approval-lead">有设备正在登录你的账号，请确认是否本人操作。</p>
      <div class="approval-target">
        <div class="approval-device">{{ deviceLabel(current.user_agent) }}</div>
        <div class="approval-meta">{{ current.ip || '未知 IP' }} · {{ formatDate(current.created_at) }}</div>
      </div>
      <p v-if="requests.length > 1" class="approval-more">
        还有 {{ requests.length - 1 }} 个待确认请求。
      </p>
      <p class="approval-risk">
        如果不是你本人操作，请点“拒绝”，并尽快修改密码。
      </p>
      <div v-if="error" class="alert alert-danger">{{ error }}</div>
      <div class="approval-actions">
        <button class="btn btn-primary" type="button" :disabled="busy" @click="decide(true)">
          {{ busy ? '处理中…' : '允许登录' }}
        </button>
        <button class="btn btn-secondary" type="button" :disabled="busy" @click="decide(false)">
          拒绝
        </button>
      </div>
    </div>
  </Modal>
</template>

<script setup>
import { onMounted, onUnmounted, ref } from 'vue'
import Modal from './Modal.vue'
import { api } from '../api'
import { store } from '../store'

const requests = ref([])
const current = ref(null)
const busy = ref(false)
const error = ref('')
const snoozed = ref({})
let timer = null
let polling = false

function deviceLabel(uaValue) {
  const ua = String(uaValue || '')
  if (!ua) return '未知设备'
  const browser = /MicroMessenger/i.test(ua) ? '微信'
    : /Edg\//i.test(ua) ? 'Edge'
      : /CriOS|Chrome\//i.test(ua) ? 'Chrome'
        : /Firefox\//i.test(ua) ? 'Firefox'
          : /Safari\//i.test(ua) ? 'Safari' : '浏览器'
  const os = /Android/i.test(ua) ? 'Android'
    : /iPhone|iPad|iPod/i.test(ua) ? 'iOS'
      : /Windows/i.test(ua) ? 'Windows'
        : /Mac OS X/i.test(ua) ? 'macOS' : ''
  return os ? `${browser} · ${os}` : browser
}

function formatDate(value) {
  if (!value) return ''
  return String(value).replace('T', ' ').replace('Z', '').slice(0, 16)
}

function snooze() {
  if (!current.value) return
  snoozed.value[current.value.id] = Date.now() + 30000
  current.value = null
}

async function poll() {
  if (polling || !store.user) return
  polling = true
  try {
    const data = await api('/api/auth/step-up/device/pending')
    const list = data.requests || []
    requests.value = list
    if (current.value && !list.some((item) => item.id === current.value.id)) {
      current.value = null
    }
    if (!current.value) {
      const now = Date.now()
      const next = list.find((item) => !snoozed.value[item.id] || snoozed.value[item.id] < now)
      if (next) {
        current.value = next
        error.value = ''
      }
    }
  } catch {
    // A transient network error should not interrupt the user's page.
  } finally {
    polling = false
  }
}

async function decide(approve) {
  if (!current.value || busy.value) return
  busy.value = true
  error.value = ''
  try {
    await api(`/api/auth/step-up/device/${current.value.id}/decide`, {
      method: 'POST',
      body: { approve }
    })
    requests.value = requests.value.filter((item) => item.id !== current.value.id)
    current.value = null
    await poll()
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

onMounted(() => {
  poll()
  timer = setInterval(poll, 3500)
})

onUnmounted(() => {
  if (timer) clearInterval(timer)
})
</script>

<style scoped>
.approval-lead {
  margin: 0 0 14px;
  font-size: 13px;
  line-height: 1.7;
  color: var(--ink-soft);
}
.approval-target {
  padding: 12px 14px;
  border: 1px solid var(--line);
  border-radius: 12px;
  background: var(--fill);
}
.approval-device { font-size: 13px; color: var(--ink); }
.approval-meta { margin-top: 3px; font-size: 11.5px; color: var(--ink-faint); }
.approval-more { margin: 10px 0 0; font-size: 11.5px; color: var(--ink-faint); }
.approval-risk {
  margin: 12px 0 0;
  font-size: 11.5px;
  line-height: 1.7;
  color: var(--danger);
}
.approval-actions {
  display: flex;
  gap: 10px;
  margin-top: 18px;
}
.approval-actions .btn { flex: 1; }
</style>
