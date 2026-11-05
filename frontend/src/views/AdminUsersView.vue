<template>
  <div>
    <div class="page-head">
      <div>
        <h1 class="page-title">用户管理</h1>
        <p class="page-sub">注册用户与角色。</p>
      </div>
      <button class="btn btn-primary" type="button" @click="openCreate">添加用户</button>
    </div>

    <div v-if="error" class="alert alert-danger">{{ error }}</div>
    <div v-if="loading" class="loading-line"><span class="spinner"></span>加载中…</div>

    <div v-else class="panel">
      <div class="table-wrap">
        <table class="table">
          <thead>
            <tr><th>用户</th><th>邮箱</th><th>角色</th><th>状态</th><th>最后登录</th><th>操作</th></tr>
          </thead>
          <tbody>
            <tr v-for="u in users" :key="u.id">
              <td>
                <div class="cell-ident">
                  <span class="user-avatar">{{ (u.full_name || u.username || u.email || '?').slice(0, 1).toUpperCase() }}</span>
                  <div class="cell-ident-text">
                    <div class="cell-main">{{ u.username || '未设置' }}</div>
                    <div v-if="u.full_name" class="cell-sub">{{ u.full_name }}</div>
                  </div>
                </div>
              </td>
              <td>{{ u.email || '-' }}</td>
              <td>
                <div class="tag-row">
                  <span v-if="u.is_super_admin" class="badge badge-super">超级管理员</span>
                  <span v-else-if="u.is_admin" class="badge badge-admin">管理员</span>
                </div>
              </td>
              <td>
                <span class="badge" :class="u.is_active ? 'badge-success' : 'badge-secondary'">
                  {{ u.is_active ? '正常' : '已禁用' }}
                </span>
              </td>
              <td class="text-secondary">{{ u.last_login_at ? formatDate(u.last_login_at) : '从未登录' }}</td>
              <td>
                <div class="row-actions">
                  <template v-if="u.id !== me?.id">
                    <button class="btn btn-secondary btn-sm" type="button" @click="toggleSuper(u)">
                      {{ u.is_super_admin ? '取消超管' : '设为超管' }}
                    </button>
                    <button class="btn btn-secondary btn-sm" type="button" @click="toggle(u)">
                      {{ u.is_active ? '禁用' : '启用' }}
                    </button>
                    <button class="btn btn-danger btn-sm" type="button" @click="remove(u)">删除</button>
                  </template>
                  <span v-else class="text-xs text-muted">当前用户</span>
                </div>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>

    <Modal :open="showCreate" @close="showCreate = false">
      <template #title>添加用户</template>
      <form @submit.prevent="createUser">
        <div class="form-group">
          <label class="form-label">用户名 *</label>
          <input v-model.trim="form.username" class="form-input" required minlength="3" placeholder="用户名" />
        </div>
        <div class="form-group">
          <label class="form-label">密码 *</label>
          <input v-model="form.password" class="form-input" type="password" required minlength="8"
                 placeholder="至少 8 位，含两类字符" />
        </div>
        <div class="form-group">
          <label class="form-label">邮箱</label>
          <input v-model.trim="form.email" class="form-input" type="email" placeholder="xxx@example.com" />
        </div>
        <div class="form-group">
          <label class="form-label">昵称</label>
          <input v-model.trim="form.fullName" class="form-input" placeholder="显示名称" />
        </div>
        <div class="form-group" style="margin-bottom: 0;">
          <label class="checkbox-line">
            <input v-model="form.isAdmin" type="checkbox" />
            <span>设为管理员</span>
          </label>
        </div>
        <div class="modal-actions">
          <button class="btn btn-secondary" type="button" @click="showCreate = false">取消</button>
          <button class="btn btn-primary" type="submit" :disabled="submitting">
            {{ submitting ? '创建中…' : '创建' }}
          </button>
        </div>
      </form>
    </Modal>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import Modal from '../components/Modal.vue'
import { api } from '../api'
import { store } from '../store'

const users = ref([])
const loading = ref(true)
const error = ref('')
const submitting = ref(false)
const showCreate = ref(false)
const me = computed(() => store.user)

const form = reactive({
  username: '',
  password: '',
  email: '',
  fullName: '',
  isAdmin: false
})

function formatDate(value) {
  if (!value) return '-'
  return String(value).replace('T', ' ').replace('Z', '').slice(0, 16)
}

async function load() {
  loading.value = true
  error.value = ''
  try {
    const data = await api('/api/admin/users')
    users.value = data?.users || []
  } catch (e) {
    error.value = e.message
  } finally {
    loading.value = false
  }
}

function openCreate() {
  form.username = ''
  form.password = ''
  form.email = ''
  form.fullName = ''
  form.isAdmin = false
  showCreate.value = true
}

async function createUser() {
  error.value = ''
  submitting.value = true
  try {
    await api('/api/admin/users', {
      method: 'POST',
      body: {
        username: form.username,
        password: form.password,
        email: form.email,
        full_name: form.fullName,
        is_admin: form.isAdmin
      }
    })
    showCreate.value = false
    await load()
  } catch (e) {
    error.value = e.message
  } finally {
    submitting.value = false
  }
}

async function toggleSuper(u) {
  const next = !u.is_super_admin
  const name = u.full_name || u.username || u.email
  const message = next
    ? `将「${name}」设为超级管理员？超级管理员可以管理所有用户、组织和应用。`
    : `取消「${name}」的超级管理员身份？`
  if (!confirm(message)) return
  error.value = ''
  try {
    await api('/api/admin/users/' + u.id, {
      method: 'PUT',
      body: { is_super_admin: next }
    })
    await load()
  } catch (e) {
    error.value = e.message
  }
}

async function toggle(u) {
  const action = u.is_active ? '禁用' : '启用'
  if (!confirm(`确定要${action}用户「${u.username}」吗？`)) return
  error.value = ''
  try {
    await api('/api/admin/users/' + u.id, {
      method: 'PUT',
      body: { is_active: !u.is_active }
    })
    await load()
  } catch (e) {
    error.value = e.message
  }
}

async function remove(u) {
  if (!confirm(`确定要删除用户「${u.username}」吗？此操作不可撤销。`)) return
  error.value = ''
  try {
    await api('/api/admin/users/' + u.id, { method: 'DELETE' })
    await load()
  } catch (e) {
    error.value = e.message
  }
}

onMounted(load)
</script>

<style scoped>
.badge-super {
  border-color: var(--brand);
  background: var(--brand);
  color: #fff;
}
</style>
