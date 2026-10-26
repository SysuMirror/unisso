// Forward browser errors to the backend so they land in /home/cloud/deploy/logs.
const ENDPOINT = '/api/logs/client'
let lastSent = 0

function send(level, message, stack) {
  const now = Date.now()
  if (now - lastSent < 500) return
  lastSent = now
  const payload = JSON.stringify({
    level,
    message: String(message || '').slice(0, 1000),
    url: location.href.slice(0, 500),
    stack: stack ? String(stack).slice(0, 2000) : ''
  })
  try {
    if (navigator.sendBeacon) {
      navigator.sendBeacon(ENDPOINT, new Blob([payload], { type: 'application/json' }))
    } else {
      fetch(ENDPOINT, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        credentials: 'same-origin',
        keepalive: true,
        body: payload
      })
    }
  } catch {
    // Never let logging break the page.
  }
}

window.addEventListener('error', (event) => {
  send('error', event.message, event.error?.stack)
})

window.addEventListener('unhandledrejection', (event) => {
  const reason = event.reason
  send('error', 'unhandledrejection: ' + (reason?.message || reason), reason?.stack)
})

const originalConsoleError = console.error.bind(console)
console.error = (...args) => {
  const message = args.map((arg) => {
    if (typeof arg === 'string') return arg
    if (arg?.message) return arg.message
    try { return JSON.stringify(arg) } catch { return String(arg) }
  }).join(' ')
  send('error', message)
  originalConsoleError(...args)
}

export function logEvent(level, message) {
  send(level, message)
}
