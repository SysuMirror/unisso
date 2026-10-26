<template>
  <div>
    <div class="page-head">
      <div>
        <h1 class="page-title">我的授权</h1>
        <p class="page-sub">查看并撤销应用授权。</p>
      </div>
    </div>

    <div v-if="error" class="alert alert-danger">{{ error }}</div>

    <div v-if="loading" class="loading-line"><span class="spinner"></span>加载中…</div>

    <div v-else-if="consents.length" class="panel">
      <div class="table-wrap">
        <table class="table">
          <thead>
            <tr><th>应用</th><th>可访问的信息</th><th>授权时间</th><th>操作</th></tr>
          </thead>
          <tbody>
            <tr v-for="consent in consents" :key="consent.id">
              <td>
                <div class="cell-ident">
                  <img v-if="consent.application.icon" :src="consent.application.icon" alt="" class="app-icon" />
                  <span v-else class="app-icon">{{ (consent.application.name || '?').slice(0, 1) }}</span>
                  <div class="cell-ident-text">
                    <div class="cell-main">{{ consent.application.name }}</div>
                    <div v-if="consent.application.description" class="cell-sub">{{ consent.application.description }}</div>
                  </div>
                </div>
              </td>
              <td>
                <div class="tag-row">
                  <span v-for="scope in consent.scopes" :key="scope" class="tag">{{ scopeLabel(scope) }}</span>
                </div>
              </td>
              <td class="text-secondary">{{ formatDate(consent.granted_at) }}</td>
              <td>
                <button class="btn btn-danger btn-sm" :disabled="revoking === consent.id" @click="revoke(consent)">
                  {{ revoking === consent.id ? '撤销中…' : '撤销' }}
                </button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>

    <div v-else class="panel">
      <div class="empty-state">
        <h3>暂无授权记录</h3>
        <p>暂无授权记录。</p>
      </div>
    </div>
  </div>
</template>

<script setup>
import { onMounted, ref } from 'vue'
import { api } from '../api'

const consents = ref([])
const loading = ref(true)
const revoking = ref('')
const error = ref('')

const scopeLabels = {
  openid: '基础身份',
  'profile:username': '昵称',
  'profile:name': '姓名',
  'profile:avatar': '头像',
  'profile:student_id': '学号',
  email: '邮箱',
  roles: '角色'
}

function scopeLabel(scope) {
  return scopeLabels[scope] || '其他信息'
}

function formatDate(value) {
  if (!value) return '-'
  return String(value).replace('T', ' ').replace('Z', '').slice(0, 16)
}

async function load() {
  loading.value = true
  error.value = ''
  try {
    const data = await api('/api/consents')
    consents.value = data?.consents || []
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

async function revoke(consent) {
  if (!confirm(`确定要撤销「${consent.application.name}」的授权吗？`)) return
  revoking.value = consent.id
  error.value = ''
  try {
    await api('/api/consents/' + consent.id, { method: 'DELETE' })
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    revoking.value = ''
  }
}

onMounted(load)
</script>
