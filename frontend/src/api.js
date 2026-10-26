// Same-origin API client with session cookies and CSRF handling.
let csrfToken = null

export function resetCsrf() {
  csrfToken = null
}

async function fetchCsrf() {
  const res = await fetch('/api/csrf-token', {
    credentials: 'same-origin',
    cache: 'no-store'
  })
  if (!res.ok) throw new Error('安全校验暂不可用，请刷新后重试')
  const data = await res.json()
  csrfToken = data.csrf_token
  return csrfToken
}

export async function api(path, options = {}) {
  const { method = 'GET', body, headers = {}, retry = true } = options
  const opts = {
    method,
    credentials: 'same-origin',
    headers: { Accept: 'application/json', ...headers }
  }
  if (body !== undefined) {
    if (body instanceof FormData) {
      opts.body = body
    } else {
      opts.headers['Content-Type'] = 'application/json'
      opts.body = JSON.stringify(body)
    }
  }
  if (!['GET', 'HEAD', 'OPTIONS'].includes(method.toUpperCase())) {
    opts.headers['X-CSRF-Token'] = csrfToken || (await fetchCsrf())
  }

  const res = await fetch(path, opts)
  const text = await res.text()
  let data = null
  try {
    data = text ? JSON.parse(text) : null
  } catch {
    data = { message: text }
  }

  if (!res.ok) {
    if (res.status === 403 && data?.error === 'csrf_validation_failed' && retry) {
      csrfToken = null
      return api(path, { ...options, retry: false })
    }
    const message =
      data?.message ||
      data?.error_description ||
      data?.error ||
      `请求失败 (${res.status})`
    const error = new Error(message)
    error.status = res.status
    error.data = data
    throw error
  }
  return data
}
