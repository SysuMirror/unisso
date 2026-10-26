<template>
  <section class="section">
    <div class="section-head">
      <div>
        <h2 class="section-title">登录设备</h2>
        <p class="section-sub">查看当前账号的登录状态，可退出其他设备。</p>
      </div>
      <button v-if="otherCount" class="btn btn-secondary btn-sm" type="button"
              :disabled="busy" @click="revokeOthers">
        退出其他设备
      </button>
    </div>

    <div v-if="error" class="alert alert-danger">{{ error }}</div>
    <div v-if="notice" class="alert alert-success">{{ notice }}</div>

    <div class="device-trust">
      <div class="device-trust-main">
        <div class="device-trust-title">
          当前设备{{ trust.trusted ? '已开启长期信任' : '未开启长期信任' }}
        </div>
        <p class="device-trust-copy">
          {{ trust.trusted
            ? `这台设备在 ${trust.days} 天内登录时不再要求二次验证。`
            : `下次登录仍会要求邮箱或人脸二次验证。` }}
        </p>
      </div>
      <button v-if="!trust.trusted" class="btn btn-secondary btn-sm" type="button"
              :disabled="busy" @click="trustRisk = true">
        信任此设备
      </button>
      <button v-else class="btn btn-secondary btn-sm" type="button"
              :disabled="busy" @click="disableTrust">
        取消信任
      </button>
    </div>

    <div v-if="trustRisk" class="device-trust-risk">
      <p class="device-trust-risk-text">
        只在你自己的设备上开启。开启后，这台设备登录时不再要求邮箱或人脸验证；
        任何能使用这台设备的人都可以直接进入你的账号。账号连续多天未登录时仍会要求验证。
        你可以随时在这里取消信任。
      </p>
      <div class="device-trust-actions">
        <button class="btn btn-primary btn-sm" type="button"
                :disabled="busy" @click="enableTrust">我已了解风险，信任此设备</button>
        <button class="btn btn-ghost btn-sm" type="button"
                :disabled="busy" @click="trustRisk = false">取消</button>
      </div>
    </div>

    <div v-if="loading" class="loading-line"><span class="spinner"></span>加载中…</div>

    <div v-else-if="sessions.length" class="device-list">
      <div v-for="session in sessions" :key="session.id" class="device-row">
        <div class="device-icon" aria-hidden="true">{{ deviceIcon(session.user_agent) }}</div>
        <div class="device-main">
          <div class="device-title">
            {{ deviceLabel(session.user_agent) }}
            <span v-if="session.current" class="device-current">当前设备</span>
          </div>
          <div class="device-meta">
            {{ session.ip || '未知 IP' }}
            <span v-if="session.created_at"> · 登录于 {{ formatDate(session.created_at) }}</span>
          </div>
          <div v-if="session.last_seen_at" class="device-meta">
            最近活动 {{ formatDate(session.last_seen_at) }}
          </div>
        </div>
        <div class="device-action">
          <span v-if="session.current" class="text-xs text-muted">正在使用</span>
          <button v-else class="btn btn-danger btn-sm" type="button"
                  :disabled="busy" @click="revoke(session)">退出</button>
        </div>
      </div>
    </div>

    <div v-else-if="!loading" class="compact-empty">当前只有这一台设备在登录。</div>

    <p class="footnote">
      退出设备会立即下线该会话，并移除它的长期信任，下次登录需要重新验证；
      同一设备还有其他登录会话时会保留信任。当前设备请使用页面右上角的退出登录。
    </p>
  </section>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { api } from '../api'

const sessions = ref([])
const trust = ref({ trusted: false, days: 180, last_seen_at: '' })
const trustRisk = ref(false)
const loading = ref(true)
const busy = ref(false)
const error = ref('')
const notice = ref('')

const otherCount = computed(() => sessions.value.filter((session) => !session.current).length)

function deviceLabel(uaValue) {
  const ua = String(uaValue || '')
  if (!ua) return '旧登录 · 设备信息不可用'
  const browser = /MicroMessenger/i.test(ua)
    ? '微信'
    : /Edg\//i.test(ua)
      ? 'Edge'
      : /CriOS|Chrome\//i.test(ua)
        ? 'Chrome'
        : /Firefox\//i.test(ua)
          ? 'Firefox'
          : /Safari\//i.test(ua)
            ? 'Safari'
            : '浏览器'
  const os = /Android/i.test(ua)
    ? 'Android'
    : /iPhone|iPad|iPod/i.test(ua)
      ? 'iOS'
      : /Windows/i.test(ua)
        ? 'Windows'
        : /Mac OS X/i.test(ua)
          ? 'macOS'
          : /Linux/i.test(ua)
            ? 'Linux'
            : ''
  return os ? `${browser} · ${os}` : browser
}

function deviceIcon(uaValue) {
  const ua = String(uaValue || '')
  if (!ua) return '旧'
  return /Android|iPhone|iPad|iPod|Mobile/i.test(ua) ? '手机' : '电脑'
}

function formatDate(value) {
  if (!value) return ''
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return String(value).replace('T', ' ').slice(0, 16)
  const pad = (n) => String(n).padStart(2, '0')
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}`
}

async function load() {
  error.value = ''
  try {
    const data = await api('/api/account/sessions')
    sessions.value = data.sessions || []
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

async function loadTrust() {
  try {
    const data = await api('/api/account/device-trust')
    trust.value = {
      trusted: !!data.trusted,
      days: Number(data.days) || 180,
      last_seen_at: data.last_seen_at || ''
    }
  } catch (e) {
    if (!error.value) error.value = e.message
  }
}

async function enableTrust() {
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    const data = await api('/api/account/device-trust', {
      method: 'POST',
      body: { trusted: true }
    })
    trust.value = {
      trusted: !!data.trusted,
      days: Number(data.days) || trust.value.days,
      last_seen_at: trust.value.last_seen_at
    }
    trustRisk.value = false
    notice.value = `已信任当前设备 ${trust.value.days} 天。`
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function disableTrust() {
  if (!window.confirm(`取消当前设备的长期信任？下次登录需要重新完成二次验证。`)) return
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    const data = await api('/api/account/device-trust', {
      method: 'POST',
      body: { trusted: false }
    })
    trust.value = {
      trusted: !!data.trusted,
      days: Number(data.days) || trust.value.days,
      last_seen_at: ''
    }
    notice.value = '已取消当前设备的长期信任。'
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function revoke(session) {
  if (!window.confirm(`退出「${deviceLabel(session.user_agent)}」？该设备会立即下线并移除长期信任，下次登录需要重新验证。`)) return
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    await api(`/api/account/sessions/${session.id}`, { method: 'DELETE' })
    notice.value = '该设备已退出登录。'
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

async function revokeOthers() {
  if (!window.confirm(`退出其他 ${otherCount.value} 台设备？这些设备会立即下线并移除长期信任，下次登录需要重新验证。`)) return
  error.value = ''
  notice.value = ''
  busy.value = true
  try {
    const data = await api('/api/account/sessions/revoke-others', { method: 'POST' })
    notice.value = `已退出 ${data.revoked || 0} 台设备。`
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

onMounted(() => {
  load()
  loadTrust()
})
</script>

<style scoped>
.device-trust {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 14px;
  padding: 14px 18px;
  border-top: 1px solid var(--line);
  border-bottom: 1px solid var(--line);
  background: var(--fill);
}
.device-trust-main { min-width: 0; }
.device-trust-title { font-size: 13px; color: var(--ink); }
.device-trust-copy { margin: 4px 0 0; font-size: 11.5px; line-height: 1.7; color: var(--ink-faint); }
.device-trust-risk {
  padding: 12px 18px;
  border-bottom: 1px solid var(--line);
  border-left: 2px solid var(--warn);
  background: var(--warn-soft);
}
.device-trust-risk-text {
  margin: 0;
  font-size: 11.5px;
  line-height: 1.75;
  color: var(--warn);
}
.device-trust-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 10px;
}
.device-list { border-top: 1px solid var(--line); }
.device-row {
  display: grid;
  grid-template-columns: 42px minmax(0, 1fr) auto;
  gap: 14px;
  align-items: center;
  padding: 14px 18px;
  border-bottom: 1px solid var(--line);
}
.device-icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 42px;
  height: 42px;
  border: 1px solid var(--line);
  font-size: 10px;
  color: var(--ink-faint);
}
.device-main { min-width: 0; }
.device-title {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13.5px;
  color: var(--ink);
}
.device-current {
  padding: 2px 6px;
  border: 1px solid var(--line);
  font-size: 10px;
  color: var(--ink-soft);
}
.device-meta {
  margin-top: 3px;
  font-size: 11.5px;
  color: var(--ink-faint);
  overflow-wrap: anywhere;
}
.device-action { display: flex; justify-content: flex-end; }
.footnote { margin: 12px 18px 16px; font-size: 11.5px; line-height: 1.7; color: var(--ink-faint); }
@media (max-width: 640px) {
  .device-trust {
    flex-direction: column;
    align-items: flex-start;
    padding: 12px 14px;
  }
  .device-row {
    grid-template-columns: 36px minmax(0, 1fr);
    gap: 10px;
    padding: 12px 14px;
  }
  .device-icon { width: 36px; height: 36px; }
  .device-action {
    grid-column: 2;
    justify-content: flex-start;
  }
  .footnote { margin: 12px 14px 16px; }
}
</style>
