<template>
  <div>
    <div class="page-head">
      <div>
        <h1 class="page-title">首页配置</h1>
        <p class="page-sub">
          编辑首页「已接入的生态」里滚动的应用：图标、跳转链接与介绍语。保存后即时生效。
        </p>
      </div>
      <div class="page-head-actions">
        <RouterLink to="/" class="btn btn-secondary">查看首页</RouterLink>
        <button class="btn btn-primary" type="button" @click="openCreate">添加应用</button>
      </div>
    </div>

    <div v-if="error" class="alert alert-danger">{{ error }}</div>
    <div v-if="loading" class="loading-line"><span class="spinner"></span>加载中…</div>

    <div v-else class="panel">
      <div v-if="apps.length" class="table-wrap">
        <table class="table">
          <thead>
            <tr><th>顺序</th><th>应用</th><th>介绍语</th><th>链接</th><th>状态</th><th>操作</th></tr>
          </thead>
          <tbody>
            <tr v-for="(app, index) in apps" :key="app.id">
              <td>
                <div class="order-cell">
                  <button class="order-btn" type="button" aria-label="上移"
                          :disabled="busy || index === 0" @click="move(app, -1)">↑</button>
                  <button class="order-btn" type="button" aria-label="下移"
                          :disabled="busy || index === apps.length - 1" @click="move(app, 1)">↓</button>
                </div>
              </td>
              <td>
                <div class="cell-ident">
                  <img v-if="app.icon" :src="app.icon" class="app-icon" alt="" />
                  <span v-else class="app-icon">{{ (app.name || '?').slice(0, 1).toUpperCase() }}</span>
                  <div class="cell-ident-text">
                    <div class="cell-main">{{ app.name }}</div>
                  </div>
                </div>
              </td>
              <td>
                <div class="home-desc">{{ app.description || '—' }}</div>
              </td>
              <td>
                <a class="link-out" :href="app.url" target="_blank" rel="noopener noreferrer">
                  {{ app.url }}
                </a>
              </td>
              <td>
                <span class="badge" :class="app.is_visible ? 'badge-success' : 'badge-secondary'">
                  {{ app.is_visible ? '首页显示' : '已隐藏' }}
                </span>
              </td>
              <td>
                <div class="row-actions">
                  <button class="btn btn-secondary btn-sm" type="button" :disabled="busy"
                          @click="toggleVisible(app)">{{ app.is_visible ? '隐藏' : '显示' }}</button>
                  <button class="btn btn-secondary btn-sm" type="button" @click="openEdit(app)">编辑</button>
                  <button class="btn btn-danger btn-sm" type="button" @click="remove(app)">删除</button>
                </div>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <div v-else class="empty-state">
        <h3>还没有配置首页应用</h3>
        <p>添加第一个应用，它就会出现在首页的滚动动画里。</p>
      </div>
    </div>

    <Modal :open="showForm" @close="showForm = false">
      <template #title>{{ form.id ? '编辑首页应用' : '添加首页应用' }}</template>
      <form @submit.prevent="save">
        <div class="form-group">
          <label class="form-label">应用名称 *</label>
          <input v-model.trim="form.name" class="form-input" required placeholder="例如：集市招聘" />
        </div>
        <div class="form-group">
          <label class="form-label">跳转链接 *</label>
          <input v-model.trim="form.url" class="form-input" type="url" required
                 placeholder="https://your-app.com" />
          <p class="form-hint">必须使用 https://（本地调试可用 http://localhost）。点击图标会新窗口打开。</p>
        </div>
        <div class="form-group">
          <label class="form-label">介绍语</label>
          <textarea v-model.trim="form.description" class="form-input" rows="2"
                    placeholder="一句话说明这个应用是做什么的"></textarea>
        </div>
        <div class="form-group">
          <label class="form-label">图标</label>
          <div class="icon-row">
            <span class="icon-preview">
              <img v-if="form.icon" :src="form.icon" alt="" />
              <template v-else>{{ (form.name || '?').slice(0, 1).toUpperCase() }}</template>
            </span>
            <div class="icon-fields">
              <input v-model.trim="form.icon" class="form-input" placeholder="图标 URL（可留空，显示首字母）" />
              <div v-if="form.id" class="icon-upload">
                <label class="btn btn-secondary btn-sm" for="hp-icon-file">
                  {{ uploading ? '上传中…' : '上传图片' }}
                </label>
                <input id="hp-icon-file" class="file-input" type="file" accept="image/*"
                       :disabled="uploading" @change="uploadIcon" />
                <span class="form-hint">JPEG / PNG / GIF / WebP，≤2MB</span>
              </div>
              <p v-else class="form-hint">先保存，再回到编辑里上传图片。</p>
            </div>
          </div>
        </div>
        <div class="form-group" style="margin-bottom: 0;">
          <label class="checkbox-line">
            <input v-model="form.is_visible" type="checkbox" />
            <span>在首页显示</span>
          </label>
        </div>
        <div class="modal-actions">
          <button class="btn btn-secondary" type="button" @click="showForm = false">取消</button>
          <button class="btn btn-primary" type="submit" :disabled="submitting">
            {{ submitting ? '保存中…' : '保存' }}
          </button>
        </div>
      </form>
    </Modal>
  </div>
</template>

<script setup>
import { onMounted, reactive, ref } from 'vue'
import Modal from '../components/Modal.vue'
import { api } from '../api'

const apps = ref([])
const loading = ref(true)
const busy = ref(false)
const submitting = ref(false)
const uploading = ref(false)
const error = ref('')
const showForm = ref(false)

const form = reactive({
  id: '',
  name: '',
  url: '',
  description: '',
  icon: '',
  is_visible: true
})

async function load() {
  loading.value = true
  error.value = ''
  try {
    const data = await api('/api/admin/homepage-apps')
    apps.value = data?.applications || []
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

function openCreate() {
  Object.assign(form, { id: '', name: '', url: '', description: '', icon: '', is_visible: true })
  showForm.value = true
}

function openEdit(app) {
  Object.assign(form, {
    id: app.id,
    name: app.name || '',
    url: app.url || '',
    description: app.description || '',
    icon: app.icon || '',
    is_visible: !!app.is_visible
  })
  showForm.value = true
}

async function save() {
  error.value = ''
  submitting.value = true
  try {
    const body = {
      name: form.name,
      url: form.url,
      description: form.description,
      icon: form.icon,
      is_visible: form.is_visible
    }
    if (form.id) {
      await api('/api/admin/homepage-apps/' + form.id, { method: 'PUT', body })
    } else {
      await api('/api/admin/homepage-apps', { method: 'POST', body })
    }
    showForm.value = false
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    submitting.value = false
  }
}

async function toggleVisible(app) {
  error.value = ''
  busy.value = true
  try {
    await api('/api/admin/homepage-apps/' + app.id, {
      method: 'PUT',
      body: { is_visible: !app.is_visible }
    })
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    busy.value = false
  }
}

// Swap positions with the neighbour: the list is ordered by sort_order, so
// exchanging the two values is enough.
async function move(app, direction) {
  const index = apps.value.findIndex((item) => item.id === app.id)
  const other = apps.value[index + direction]
  if (!other) return
  error.value = ''
  busy.value = true
  try {
    const lower = direction < 0 ? other : app
    const upper = direction < 0 ? app : other
    await api('/api/admin/homepage-apps/' + upper.id,
      { method: 'PUT', body: { sort_order: lower.sort_order } })
    await api('/api/admin/homepage-apps/' + lower.id,
      { method: 'PUT', body: { sort_order: upper.sort_order } })
    await load()
  } catch (e) {
    error.value = e.message
    await load()
  } finally {
    busy.value = false
  }
}

async function uploadIcon(event) {
  const file = event.target.files?.[0]
  event.target.value = ''
  if (!file || !form.id) return
  error.value = ''
  uploading.value = true
  try {
    const data = new FormData()
    data.append('file', file)
    const result = await api('/api/admin/homepage-apps/' + form.id + '/icon', {
      method: 'POST',
      body: data
    })
    form.icon = result.url
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    uploading.value = false
  }
}

async function remove(app) {
  if (!confirm(`确定把「${app.name}」从首页移除吗？`)) return
  error.value = ''
  try {
    await api('/api/admin/homepage-apps/' + app.id, { method: 'DELETE' })
    await load()
  } catch (e) {
    error.value = e.message
  }
}

onMounted(load)
</script>

<style scoped>
.page-head-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.order-cell {
  display: flex;
  gap: 4px;
}
.order-btn {
  width: 26px;
  height: 26px;
  border: 1px solid var(--line);
  border-radius: var(--radius-sm);
  background: var(--surface);
  color: var(--ink-soft);
  font-size: 12px;
  cursor: pointer;
}
.order-btn:hover:not(:disabled) {
  border-color: var(--line-strong);
  color: var(--ink);
}
.order-btn:disabled {
  opacity: .35;
  cursor: not-allowed;
}
.home-desc {
  max-width: 320px;
  font-size: 12.5px;
  color: var(--ink-soft);
  line-height: 1.7;
}
.link-out {
  display: inline-block;
  max-width: 240px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  vertical-align: bottom;
  font-family: var(--mono);
  font-size: 11.5px;
  color: var(--brand-ink);
}
.link-out:hover { text-decoration: underline; text-underline-offset: 3px; }
.icon-row {
  display: flex;
  gap: 14px;
  align-items: flex-start;
}
.icon-preview {
  width: 46px;
  height: 46px;
  flex: 0 0 auto;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: var(--fill);
  font-size: 17px;
  font-weight: 600;
  color: var(--ink-soft);
  overflow: hidden;
}
.icon-preview img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.icon-fields {
  flex: 1;
  min-width: 0;
}
.icon-upload {
  display: flex;
  align-items: center;
  gap: 10px;
  margin-top: 8px;
  flex-wrap: wrap;
}
.file-input { display: none; }
</style>
