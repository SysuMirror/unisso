<template>
  <AuthShell kicker="登录确认">
    <div class="qr-confirm">
      <div v-if="error" class="auth-error">{{ error }}</div>

      <div v-if="loading" class="auth-qr-hint">加载中…</div>

      <template v-else-if="status === 'pending' || status === 'scanned'">
        <div class="qr-device-card">
          <span class="qr-device-icon" aria-hidden="true">
            <svg viewBox="0 0 24 24" width="26" height="26" fill="none"
                 stroke="currentColor" stroke-width="1.6" stroke-linecap="round"
                 stroke-linejoin="round">
              <rect x="3" y="4" width="18" height="12" rx="2" />
              <path d="M8 20h8M12 16v4" />
            </svg>
          </span>
          <div class="qr-device-text">
            <div class="qr-device-title">{{ deviceLabel }}</div>
            <div class="qr-device-meta" v-if="requestedLabel">发起时间 {{ requestedLabel }}</div>
            <div class="qr-device-meta" v-if="device.ip">IP {{ device.ip }}</div>
          </div>
        </div>
        <p class="qr-account">你的手机当前登录：<strong>{{ accountName }}</strong></p>
        <p class="qr-warn">确认是你在电脑上发起的登录吗？如果不是，请选择拒绝。</p>
        <button class="auth-submit" type="button" :disabled="submitting" @click="decide('confirm')">
          {{ submitting ? '处理中…' : '确认登录' }}
        </button>
        <div class="qr-deny-row">
          <button class="auth-alt-link" type="button" :disabled="submitting" @click="decide('deny')">
            拒绝本次登录
          </button>
        </div>
      </template>

      <div v-else-if="status === 'done'" class="qr-state">
        <p class="qr-state-title">已确认，请回到电脑继续</p>
        <p class="qr-sub-hint">本页面可以关闭。</p>
      </div>
      <div v-else-if="status === 'denied'" class="qr-state">
        <p class="qr-state-title">已拒绝本次登录</p>
        <p class="qr-sub-hint">如需登录，请回到电脑重新扫码。</p>
      </div>
      <div v-else class="qr-state">
        <p class="qr-state-title">二维码已失效</p>
        <p class="qr-sub-hint">请回到电脑上刷新二维码后重新扫码。</p>
      </div>
    </div>
  </AuthShell>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { useRoute } from 'vue-router'
import AuthShell from '../components/AuthShell.vue'
import { api } from '../api'
import { store } from '../store'

const route = useRoute()
const id = String(route.params.id || '')
const loading = ref(true)
const submitting = ref(false)
const error = ref('')
const status = ref('expired')
const device = ref({})

const deviceLabel = computed(() => {
  const parts = [device.value.os, device.value.browser].filter(Boolean)
  return parts.length ? parts.join(' · ') + ' 电脑' : '一台电脑'
})

const requestedLabel = computed(() => {
  const epoch = Number(device.value.requested_epoch || 0)
  if (epoch) {
    const d = new Date(epoch * 1000)
    const pad = (n) => String(n).padStart(2, '0')
    const sameDay = new Date().toDateString() === d.toDateString()
    const hm = `${pad(d.getHours())}:${pad(d.getMinutes())}`
    return sameDay ? `今天 ${hm}` : `${d.getMonth() + 1}月${d.getDate()}日 ${hm}`
  }
  const v = device.value.requested_at
  return v ? String(v).replace('T', ' ').replace('Z', '').slice(0, 16) : ''
})

const accountName = computed(() => {
  const user = store.user
  if (!user) return '未知账号'
  return user.full_name || user.username || user.email || '未知账号'
})

async function load() {
  loading.value = true
  error.value = ''
  try {
    const data = await api('/api/auth/qr/detail?id=' + encodeURIComponent(id))
    status.value = data.status === 'confirmed' ? 'done' : data.status
    device.value = data.device || {}
  } catch (e) {
    error.value = e.message
    status.value = 'expired'
  } finally {
    loading.value = false
  }
}

async function decide(action) {
  submitting.value = true
  error.value = ''
  try {
    await api('/api/auth/qr/' + action, { method: 'POST', body: { id } })
    status.value = action === 'confirm' ? 'done' : 'denied'
  } catch (e) {
    if (e.status === 409 || e.status === 410) {
      await load()
    } else {
      error.value = e.message
    }
  } finally {
    submitting.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.qr-confirm { margin-top: 4px; }
.qr-device-card {
  display: flex;
  gap: 14px;
  align-items: center;
  padding: 16px;
  border: 1px solid var(--line, #e8e8e4);
  border-radius: 14px;
  background: var(--surface, #fbfbfa);
}
.qr-device-icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 44px;
  height: 44px;
  border-radius: 12px;
  color: var(--brand, #171717);
  background: rgba(23, 23, 23, .06);
  flex: none;
}
.qr-device-title { font-size: 15px; font-weight: 600; }
.qr-device-meta { font-size: 12px; color: var(--ink-soft, #8a8a86); margin-top: 3px; }
.qr-account { font-size: 13px; margin: 16px 0 4px; }
.qr-warn { font-size: 13px; color: var(--ink-soft, #666); margin: 0 0 18px; line-height: 1.7; }
.qr-deny-row { margin-top: 10px; text-align: center; }
.qr-state { text-align: center; padding: 26px 0 8px; }
.qr-state-title { font-size: 16px; font-weight: 600; margin: 0 0 8px; }
.qr-sub-hint { font-size: 12px; color: var(--ink-soft, #8a8a86); margin: 0; line-height: 1.6; }
</style>
