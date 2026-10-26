<template>
  <div class="auth-shell">
    <img class="auth-page-logo" :src="logo" alt="统一身份认证" />

    <div class="auth-art" aria-hidden="true">
      <div class="auth-orbit orbit-1"></div>
      <div class="auth-orbit orbit-2"></div>
      <img class="auth-art-logo" :src="logo" alt="" />
    </div>

    <aside class="auth-aside">
      <blockquote class="auth-quote">
        <p>认识你自己，<br />是一切智慧的开端。</p>
        <footer>亚里士多德</footer>
      </blockquote>
    </aside>

    <main class="auth-main">
      <div class="auth-panel-flat">
        <div class="auth-mobile-brand">
          <div class="auth-logo-wrap">
            <img class="auth-logo" :src="logo" alt="统一身份认证" />
          </div>
        </div>
        <div class="auth-kicker">{{ kicker }}</div>
        <slot />
        <div class="auth-foot"><slot name="note" /></div>
      </div>
    </main>
  </div>
</template>

<script setup>
import logo from '../assets/logo.png'

defineProps({
  kicker: { type: String, default: '统一身份认证' }
})
</script>
