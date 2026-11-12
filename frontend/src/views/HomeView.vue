<template>
  <div class="home">
    <section class="hero">
      <div class="hero-aurora" aria-hidden="true">
        <span class="aurora aurora-a"></span>
        <span class="aurora aurora-b"></span>
      </div>
      <div class="hero-orbit" aria-hidden="true">
        <span class="orbit orbit-1"></span>
        <span class="orbit orbit-2"></span>
        <span class="orbit orbit-3"></span>
      </div>

      <div class="hero-inner">
        <p class="hero-eyebrow">
          <span class="eyebrow-dot"></span>
          YatSSO · 民间 NetID
        </p>
        <h1 class="hero-title">
          一个开发者自由接入的<br />
          <span class="hero-highlight">统一隐私登录生态</span>
        </h1>
        <p class="hero-sub">
          YatSSO 是我们自己搭起来的身份层。它像 NetID 一样「一次登录、处处通行」，
          但不属于任何一家平台：<b>任何开发者都能自助接入</b>，
          用户逐次授权、只交出被明确同意的字段。
        </p>

        <div class="hero-actions">
          <RouterLink to="/developer" class="btn btn-primary btn-lg">接入我的应用</RouterLink>
          <RouterLink v-if="!user" to="/login" class="btn btn-secondary btn-lg">登录 YatSSO</RouterLink>
          <RouterLink v-else to="/profile" class="btn btn-secondary btn-lg">个人中心</RouterLink>
        </div>

        <ul class="hero-facts">
          <li v-for="fact in facts" :key="fact">{{ fact }}</li>
        </ul>
      </div>
    </section>

    <section class="ecosystem">
      <div class="eco-head">
        <div>
          <h2 class="eco-title">已接入的生态</h2>
          <p class="eco-sub">点任意图标直接前往 · 这些应用都通过 YatSSO 完成单点登录</p>
        </div>
        <RouterLink to="/developer" class="eco-more">也想出现在这里 →</RouterLink>
      </div>

      <div class="marquee" :aria-label="'已接入生态：' + apps.map((a) => a.name).join('、')">
        <div v-for="(row, rowIndex) in marqueeRows" :key="rowIndex"
             class="marquee-row" :class="rowIndex % 2 ? 'marquee-reverse' : ''">
          <div class="marquee-track"
               :style="{ animationDuration: rowIndex % 2 ? '58s' : '46s', '--shift': row.shift }">
            <a v-for="(app, index) in row.items" :key="app.id + '-' + index"
               class="tile" :href="app.url" target="_blank" rel="noopener noreferrer"
               :title="app.description || app.name">
              <span class="tile-icon">
                <img v-if="app.icon" :src="app.icon" :alt="app.name" loading="lazy" />
                <template v-else>{{ (app.name || '?').slice(0, 1).toUpperCase() }}</template>
              </span>
              <span class="tile-text">
                <span class="tile-name">{{ app.name }}</span>
                <span class="tile-desc">{{ app.description }}</span>
              </span>
            </a>
          </div>
        </div>
        <span class="marquee-fade marquee-fade-left" aria-hidden="true"></span>
        <span class="marquee-fade marquee-fade-right" aria-hidden="true"></span>
      </div>
    </section>

    <section class="section-block">
      <div class="section-head">
        <div>
          <h2 class="section-title">为什么是 YatSSO</h2>
          <p class="section-sub">不是又一个账号系统，而是一层共用的身份基础设施。</p>
        </div>
      </div>
      <div class="feature-grid">
        <div class="feature">
          <span class="feature-num">01</span>
          <h3>一次登录，处处通行</h3>
          <p>
            用户在你的应用点「用 YatSSO 登录」，跳转过来确认一次，就带着
            <code class="code-inline">code</code> 回到你的后端。
            标准 OAuth2 授权码 + PKCE（S256），没有私有协议。
          </p>
        </div>
        <div class="feature">
          <span class="feature-num">02</span>
          <h3>只管身份，不管权限</h3>
          <p>
            YatSSO 只回答「这个人是谁」，角色与权限完全由你自己的系统判定。
            ID Token 用 RS256 签名，公钥通过公开的 JWKS 端点发布，可离线验签。
          </p>
        </div>
        <div class="feature">
          <span class="feature-num">03</span>
          <h3>隐私优先，字段最小化</h3>
          <p>
            每次授权都出同意页，scopes 走白名单，只返回被授权的字段；
            用来跨服务关联同一人的 <code class="code-inline">sse_sub</code>
            需要单独申请 <code class="code-inline">sse_identity</code> 才会下发。
          </p>
        </div>
      </div>
    </section>

    <section class="cta">
      <div class="cta-inner">
        <h2>把你的系统接进来</h2>
        <p>
          任何已登录账号都能自助注册接入应用，<b>提交即生效</b>，不需要等待审核。
          把接入文档丢给你的 AI 编码助手，它会照着规范把授权流程接完。
        </p>
        <div class="cta-actions">
          <RouterLink to="/developer" class="btn btn-primary btn-lg">开放平台</RouterLink>
          <RouterLink to="/docs" class="btn btn-secondary btn-lg">接入文档</RouterLink>
        </div>
      </div>
    </section>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { api } from '../api'
import { store } from '../store'

const facts = [
  '自助接入，提交即生效',
  'OAuth2 授权码 + PKCE(S256)',
  '逐次同意，字段最小化',
  'RS256 签名的 OIDC ID Token'
]

// Shown until the API answers, and kept as a safety net so the marquee is never
// empty if the public endpoint is briefly unavailable.
const FALLBACK_APPS = [
  { id: 'ssemarket', name: '集市主站', description: '校园集市：二手交易、招募、活动与社区', url: 'https://ssemarket.cn/', icon: '' },
  { id: 'join', name: '集市招聘', description: '集市招聘：岗位发布与投递', url: 'https://join.ssemarket.cn/', icon: '' },
  { id: 'iwiki', name: 'IWiki', description: '知识库与文档协作', url: 'https://iwiki.ssemarket.cn/', icon: '' },
  { id: 'cloud', name: 'SSE 云平台', description: '统一算力与部署平台：Pod、GPU、存储', url: 'https://cloud.ssemarket.cn/', icon: '' },
  { id: 'octop', name: 'Octop', description: '运维控制台，单点登录直达', url: 'https://octop.ssemarket.cn/', icon: '' },
  { id: 'turing', name: '图灵科竞协会', description: '图灵科竞协会统一平台', url: 'https://turing.ssemarket.cn/', icon: '' }
]

const apps = ref(FALLBACK_APPS)
const user = computed(() => store.user)

// Two rows scrolling in opposite directions. Each row is repeated whole times
// and the keyframe shifts by exactly one repetition, so the loop is seamless
// (--shift = 100 / repetitions %).
const marqueeRows = computed(() => {
  const list = apps.value.filter((app) => app.url)
  const source = list.length ? list : FALLBACK_APPS
  const rows = [[], []]
  source.forEach((app, index) => rows[index % 2].push(app))
  if (rows[0].length === 0) rows[0] = source
  return rows.map((row) => {
    const base = row.length ? row : source
    const repeats = Math.max(3, Math.ceil(6 / base.length))
    const items = []
    for (let i = 0; i < repeats; i += 1) items.push(...base)
    return { items, shift: (100 / repeats).toFixed(4) + '%' }
  })
})

async function load() {
  try {
    const data = await api('/api/public/homepage-apps')
    const list = (data?.applications || []).filter((app) => app.url)
    if (list.length) apps.value = list
  } catch {
    // Keep the fallback list; the landing page must still render.
  }
}

onMounted(load)
</script>

<style scoped>
/* The home page is full-bleed (see .container-home in theme.css), so every
   section re-creates the shared content measure itself. One custom property
   drives the gutters for all of them, and each breakpoint just narrows it. */
.home {
  --home-max: 1120px;
  --home-pad: 32px;
  padding: 0 0 8px;
}

/* ------------------------------- hero ------------------------------- */

.hero {
  position: relative;
  overflow: hidden;
  padding: 76px var(--home-pad) 64px;
  border-bottom: 1px solid var(--line);
  background: var(--bg);
}
.hero-inner {
  position: relative;
  z-index: 2;
  max-width: var(--home-max);
  margin: 0 auto;
}
.hero-aurora,
.hero-orbit {
  position: absolute;
  pointer-events: none;
}
.hero-aurora {
  inset: -20% -10% auto auto;
  width: 760px;
  height: 620px;
  z-index: 0;
}
.aurora {
  position: absolute;
  border-radius: 50%;
  filter: blur(70px);
}
.aurora-a {
  top: 0;
  right: 0;
  width: 460px;
  height: 460px;
  background: rgba(51, 112, 255, .16);
  animation: aurora-drift 18s var(--ease) infinite alternate;
}
.aurora-b {
  top: 150px;
  right: 240px;
  width: 380px;
  height: 380px;
  background: rgba(51, 112, 255, .09);
  animation: aurora-drift 24s var(--ease) infinite alternate-reverse;
}
@keyframes aurora-drift {
  from { transform: translate3d(0, 0, 0) scale(1); }
  to { transform: translate3d(-50px, 40px, 0) scale(1.12); }
}
.hero-orbit {
  right: -60px;
  top: 50%;
  width: min(46vw, 520px);
  aspect-ratio: 1;
  transform: translateY(-46%);
  z-index: 1;
}
.orbit {
  position: absolute;
  inset: 0;
  border: 1px solid rgba(51, 112, 255, .16);
  border-radius: 50%;
}
.orbit-1 { animation: home-spin 70s linear infinite; }
.orbit-2 {
  inset: 13%;
  border-style: dashed;
  border-color: rgba(23, 23, 23, .09);
  animation: home-spin 110s linear infinite reverse;
}
.orbit-3 {
  inset: 27%;
  border-color: rgba(51, 112, 255, .1);
  animation: home-spin 150s linear infinite;
}
.orbit::after {
  content: "";
  position: absolute;
  top: -3.5px;
  left: 50%;
  width: 6px;
  height: 6px;
  margin-left: -3px;
  border-radius: 50%;
  background: var(--brand);
  box-shadow: 0 0 0 5px rgba(51, 112, 255, .1);
}
.orbit-2::after {
  width: 4px;
  height: 4px;
  background: var(--ink-faint);
  box-shadow: none;
}
@keyframes home-spin {
  to { transform: rotate(360deg); }
}

.hero-eyebrow {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 5px 13px 5px 10px;
  border: 1px solid var(--line);
  border-radius: var(--radius-pill);
  background: var(--surface);
  font-size: 11.5px;
  letter-spacing: .08em;
  color: var(--ink-soft);
}
.eyebrow-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: var(--brand);
  box-shadow: 0 0 0 4px rgba(51, 112, 255, .12);
  animation: eyebrow-pulse 2.6s ease-in-out infinite;
}
@keyframes eyebrow-pulse {
  0%, 100% { box-shadow: 0 0 0 4px rgba(51, 112, 255, .12); }
  50% { box-shadow: 0 0 0 8px rgba(51, 112, 255, .04); }
}
.hero-title {
  margin: 22px 0 0;
  font-size: clamp(30px, 5vw, 54px);
  line-height: 1.12;
  letter-spacing: -.045em;
}
.hero-highlight {
  position: relative;
  display: inline-block;
  color: var(--brand-ink);
}
.hero-highlight::after {
  content: "";
  position: absolute;
  left: 0;
  right: 0;
  bottom: .04em;
  height: .28em;
  background: rgba(51, 112, 255, .14);
  animation: highlight-grow 1.1s var(--ease) both;
}
@keyframes highlight-grow {
  from { transform: scaleX(0); transform-origin: left; }
  to { transform: scaleX(1); transform-origin: left; }
}
.hero-sub {
  max-width: 640px;
  margin: 22px 0 0;
  font-size: 15px;
  line-height: 1.9;
  color: var(--ink-soft);
}
.hero-sub b { color: var(--ink); font-weight: 600; }
.hero-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
  margin-top: 30px;
}
.hero-facts {
  display: flex;
  flex-wrap: wrap;
  gap: 8px 22px;
  margin: 30px 0 0;
  padding: 18px 0 0;
  border-top: 1px solid var(--line);
  list-style: none;
  font-size: 12px;
  color: var(--ink-faint);
}
.hero-facts li {
  position: relative;
  padding-left: 15px;
}
.hero-facts li::before {
  content: "";
  position: absolute;
  left: 0;
  top: .62em;
  width: 5px;
  height: 5px;
  border-radius: 50%;
  background: var(--brand);
  opacity: .55;
}

/* ----------------------------- ecosystem ----------------------------- */

.ecosystem {
  padding: 56px 0 8px;
  border-bottom: 1px solid var(--line);
}
.eco-head {
  display: flex;
  align-items: flex-end;
  justify-content: space-between;
  gap: 20px;
  max-width: var(--home-max);
  margin: 0 auto 26px;
  padding: 0 var(--home-pad);
}
.eco-title {
  font-size: 22px;
  letter-spacing: -.03em;
}
.eco-sub {
  margin-top: 6px;
  font-size: 13px;
  color: var(--ink-faint);
}
.eco-more {
  flex: 0 0 auto;
  font-size: 12.5px;
  color: var(--brand-ink);
}
.eco-more:hover { text-decoration: underline; text-underline-offset: 3px; }

.marquee {
  position: relative;
  padding: 4px 0 44px;
  overflow: hidden;
  -webkit-mask-image: linear-gradient(90deg, transparent, #000 9%, #000 91%, transparent);
  mask-image: linear-gradient(90deg, transparent, #000 9%, #000 91%, transparent);
}
.marquee-row {
  display: flex;
  width: max-content;
  margin-bottom: 14px;
}
.marquee-track {
  display: flex;
  gap: 14px;
  padding-right: 14px;
  animation: marquee-left linear infinite;
  will-change: transform;
}
.marquee-reverse .marquee-track { animation-name: marquee-right; }
.marquee:hover .marquee-track,
.marquee:focus-within .marquee-track {
  animation-play-state: paused;
}
@keyframes marquee-left {
  from { transform: translate3d(0, 0, 0); }
  to { transform: translate3d(calc(-1 * var(--shift, 33.3333%)), 0, 0); }
}
@keyframes marquee-right {
  from { transform: translate3d(calc(-1 * var(--shift, 33.3333%)), 0, 0); }
  to { transform: translate3d(0, 0, 0); }
}
.marquee-fade {
  position: absolute;
  top: 0;
  bottom: 40px;
  width: 90px;
  pointer-events: none;
}
.marquee-fade-left { left: 0; background: linear-gradient(90deg, var(--bg), transparent); }
.marquee-fade-right { right: 0; background: linear-gradient(270deg, var(--bg), transparent); }

.tile {
  display: flex;
  align-items: center;
  gap: 13px;
  width: 300px;
  flex: 0 0 auto;
  padding: 14px 16px;
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: var(--surface);
  box-shadow: var(--shadow);
  transition: transform .28s var(--ease), border-color .28s var(--ease),
              box-shadow .28s var(--ease);
}
.tile:hover {
  transform: translateY(-4px);
  border-color: rgba(51, 112, 255, .38);
  box-shadow: var(--shadow-lift);
}
.tile-icon {
  width: 42px;
  height: 42px;
  flex: 0 0 auto;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border: 1px solid var(--line-soft);
  border-radius: 11px;
  background: var(--fill);
  font-size: 16px;
  font-weight: 600;
  color: var(--ink-soft);
  overflow: hidden;
}
.tile-icon img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.tile-text {
  min-width: 0;
  display: flex;
  flex-direction: column;
}
.tile-name {
  font-size: 13.5px;
  font-weight: 600;
  color: var(--ink);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.tile-desc {
  margin-top: 3px;
  font-size: 11.5px;
  color: var(--ink-faint);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

/* ------------------------------ why YatSSO ------------------------------ */
/* These classes also exist in theme.css, but with page-agnostic sizes (and no
   measure of their own) — on a full-bleed page that let the three columns run
   edge to edge. They are re-declared here with a home-scoped prefix so this
   page owns its own scale instead of inheriting whichever theme layer wins. */

.home .section-block {
  max-width: var(--home-max);
  margin: 0 auto;
  padding: 56px var(--home-pad) 8px;
}
.home .section-title {
  font-size: 22px;
  letter-spacing: -.03em;
}
.home .section-sub {
  margin-top: 6px;
  font-size: 13px;
  color: var(--ink-faint);
}
.home .feature-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 32px;
  margin-top: 28px;
}
.home .feature {
  padding-top: 18px;
  border-top: 1px solid var(--ink);
}
.home .feature h3 {
  margin-top: 8px;
  font-size: 15px;
  font-weight: 600;
}
.home .feature p {
  margin-top: 6px;
  font-size: 13px;
  line-height: 1.7;
  color: var(--ink-soft);
}

/* -------------------------------- CTA -------------------------------- */

.cta {
  max-width: var(--home-max);
  margin: 0 auto;
  padding: 56px var(--home-pad) 24px;
}
/* Deliberately narrower than the grid above: a single line of copy plus two
   buttons does not need the full 1120px, it just left the card looking empty. */
.cta-inner {
  max-width: 840px;
  margin: 0 auto;
  padding: 40px 36px;
  border: 1px solid var(--line);
  border-radius: var(--radius-lg);
  background: var(--surface);
}
.cta-inner h2 {
  font-size: 24px;
  letter-spacing: -.035em;
}
.cta-inner p {
  max-width: 620px;
  margin: 14px 0 0;
  font-size: 14px;
  line-height: 1.9;
  color: var(--ink-soft);
}
.cta-inner b { color: var(--ink); font-weight: 600; }
.cta-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
  margin-top: 26px;
}

/* --------------------------- multi-device ladder --------------------------- */
/* Large tablet / small laptop: the orbit would start crowding the headline, and
   three feature columns get too narrow to read comfortably. */
@media (max-width: 1100px) {
  .home { --home-pad: 26px; }
  .hero-orbit { opacity: .55; }
  .home .feature-grid { gap: 26px; }
}
@media (max-width: 960px) {
  .home .feature-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  /* Odd third card spans the row instead of leaving a dangling half-column. */
  .home .feature-grid > .feature:last-child { grid-column: 1 / -1; }
}
@media (max-width: 900px) {
  .home { --home-pad: 20px; }
  .hero { padding: 52px var(--home-pad) 44px; }
  .hero-orbit { display: none; }
  .hero-title { font-size: clamp(28px, 6.4vw, 42px); }
  .eco-head { flex-direction: column; align-items: flex-start; gap: 10px; }
  .home .section-block { padding: 44px var(--home-pad) 8px; }
  .cta { padding: 40px var(--home-pad) 12px; }
  .cta-inner { padding: 30px 24px; }
}
/* Phone: single column everywhere, and the primary actions become full-width
   so they stay easy to hit (theme.css already floors buttons at 44px). */
@media (max-width: 720px) {
  .hero { padding: 40px var(--home-pad) 36px; }
  .hero-sub { font-size: 14px; line-height: 1.85; }
  .hero-facts { gap: 8px 16px; font-size: 11.5px; }
  .home .feature-grid {
    grid-template-columns: minmax(0, 1fr);
    gap: 22px;
    margin-top: 22px;
  }
  .home .feature-grid > .feature:last-child { grid-column: auto; }
  .cta-inner h2 { font-size: 21px; }
  .hero-actions .btn,
  .cta-actions .btn {
    flex: 1 1 100%;
    justify-content: center;
  }
  .marquee { padding-bottom: 32px; }
}
@media (max-width: 560px) {
  .home { --home-pad: 16px; }
  /* The forced break reads as a stub line on narrow screens — let it wrap. */
  .hero-title br { display: none; }
  .hero-eyebrow { font-size: 11px; }
  .tile { width: 240px; padding: 12px 13px; gap: 11px; }
  .tile-icon { width: 38px; height: 38px; }
  .tile-desc { font-size: 11px; }
  .marquee-fade { width: 44px; }
  .cta-inner { padding: 26px 18px; }
}
@media (max-width: 380px) {
  .tile { width: 214px; }
  .hero-facts { font-size: 11px; }
}
@media (prefers-reduced-motion: reduce) {
  .marquee-track,
  .aurora,
  .orbit,
  .eyebrow-dot,
  .hero-highlight::after {
    animation: none !important;
  }
  .hero-highlight::after { transform: scaleX(1); }
}
</style>
