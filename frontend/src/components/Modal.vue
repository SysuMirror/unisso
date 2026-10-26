<template>
  <div class="modal-overlay" :class="{ active: open, 'is-wide': wide }" @click.self="$emit('close')">
    <div class="modal" :class="{ 'modal-wide': wide }" role="dialog" aria-modal="true">
      <div class="modal-header">
        <h3 class="modal-title"><slot name="title">对话框</slot></h3>
        <button class="modal-close" type="button" aria-label="关闭" @click="$emit('close')">×</button>
      </div>
      <slot />
    </div>
  </div>
</template>

<script setup>
defineProps({
  open: { type: Boolean, default: false },
  wide: { type: Boolean, default: false }
})
defineEmits(['close'])
</script>
