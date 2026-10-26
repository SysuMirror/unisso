<template>
  <div>
    <div class="page-head">
      <div>
        <h1 class="page-title">运行日志</h1>
        <p class="page-sub">后端访问/错误日志与前端错误日志。</p>
      </div>
    </div>

    <div v-if="error" class="alert alert-danger">{{ error }}</div>

    <div class="log-toolbar">
      <button v-for="file in files" :key="file.name" type="button"
              class="log-file" :class="{ active: file.name === selected }"
              @click="select(file.name)">{{ file.name }}</button>
      <select v-model.number="lines" class="log-lines" @change="loadTail">
        <option :value="200">200 行</option>
        <option :value="500">500 行</option>
        <option :value="1000">1000 行</option>
      </select>
      <button type="button" class="btn btn-secondary btn-sm" @click="loadTail">刷新</button>
    </div>

    <pre class="log-view">{{ logText || '暂无日志' }}</pre>
  </div>
</template>

<script setup>
import { onMounted, onUnmounted, ref } from 'vue'
import { api } from '../api'

const files = ref([])
const selected = ref('')
const lines = ref(200)
const logText = ref('')
const error = ref('')
let timer = null

async function loadFiles() {
  error.value = ''
  try {
    const data = await api('/api/admin/logs')
    files.value = data?.files || []
    if (!selected.value || !files.value.some((f) => f.name === selected.value)) {
      selected.value = files.value.some((f) => f.name === 'backend.log')
        ? 'backend.log'
        : (files.value[0]?.name || '')
    }
    await loadTail()
  } catch (e) {
    error.value = e.message
  }
}

async function loadTail() {
  if (!selected.value) {
    logText.value = ''
    return
  }
  try {
    const data = await api(`/api/admin/logs/${encodeURIComponent(selected.value)}?lines=${lines.value}`)
    logText.value = (data?.lines || []).join('\n')
  } catch (e) {
    error.value = e.message
  }
}

function select(name) {
  selected.value = name
  loadTail()
}

onMounted(async () => {
  await loadFiles()
  timer = setInterval(() => { loadTail() }, 5000)
})

onUnmounted(() => {
  if (timer) clearInterval(timer)
})
</script>
