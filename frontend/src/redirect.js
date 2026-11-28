// Cross-origin "return to" support for the marketplace (ssemarket.cn) and its
// subdomains. Registration/login normally stay inside UniSSO; when another
// first-party site sends a user here it can pass ?redirect_uri=<absolute URL>.
//
// Only https on an allow-listed host (plus localhost for development) is
// honored, so a crafted link cannot turn the SSO page into an open redirector.
// The marketplace and its subdomains are all operated by the same team; the
// allow-list deliberately does not accept arbitrary third-party origins.

const REDIRECT_QUERY_KEYS = ['redirect_uri', 'redirect_url', 'return_to']

function isLocalHost(hostname) {
  return hostname === 'localhost' || hostname === '127.0.0.1' || hostname === '[::1]'
}

function isAllowedHost(hostname) {
  const host = hostname.toLowerCase()
  return host === 'ssemarket.cn' || host.endsWith('.ssemarket.cn') || isLocalHost(host)
}

/**
 * Normalize a redirect target.
 * - Same-origin absolute paths ("/profile") are returned as-is (existing `next`
 *   semantics inside UniSSO).
 * - Absolute URLs are only accepted for https on ssemarket.cn / *.ssemarket.cn
 *   (or http on localhost for local development).
 * - Anything else returns "" so callers fall back to their default route.
 */
export function safeRedirect(value) {
  if (typeof value !== 'string')
    return ''
  const raw = value.trim()
  if (!raw)
    return ''
  if (raw.startsWith('/') && !raw.startsWith('//'))
    return raw

  let url
  try {
    url = new URL(raw)
  }
  catch {
    return ''
  }
  if (!isAllowedHost(url.hostname))
    return ''
  if (url.protocol !== 'https:' && !(isLocalHost(url.hostname) && url.protocol === 'http:'))
    return ''
  return url.href
}

/** Read the first valid redirect target from a vue-router query object. */
export function redirectFromQuery(query) {
  if (!query || typeof query !== 'object')
    return ''
  for (const key of REDIRECT_QUERY_KEYS) {
    const target = safeRedirect(query[key])
    if (target)
      return target
  }
  return ''
}

/**
 * Navigate to a validated redirect target.
 * Uses a full-page navigation on purpose: the target is usually another
 * origin, and the shared parent-domain SSO cookie must be sent with it.
 * Returns false when there is nothing to do.
 */
export function goToRedirect(target) {
  if (!target)
    return false
  window.location.assign(target)
  return true
}