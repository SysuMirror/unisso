<template>
  <div class="face-capture">
    <div class="face-stage">
      <video
        ref="video"
        class="face-video"
        autoplay
        playsinline
        muted
      ></video>
      <div v-if="!cameraReady && !cameraError" class="face-stage-note">
        {{ copy.openHint }}
      </div>
      <div v-if="cameraError" class="face-stage-note face-stage-error">{{ cameraError }}</div>
      <div class="face-guide" aria-hidden="true"></div>
      <div v-if="cameraReady" class="face-light" :class="lightStatus">
        {{ lightLabel }}
      </div>
      <div v-if="!capturing" class="face-guide-hint">{{ copy.guideHint }}</div>
      <div v-if="phase === 'countdown'" class="face-count">{{ countdown }}</div>
      <div v-if="capturing" class="face-progress">
        <span>{{ copy.progress }}</span>
        <span>{{ capturedCount }} / {{ frameTarget }}</span>
      </div>
    </div>

    <div class="face-instruction">
      <div class="face-action">{{ challengeLabel }}</div>
      <div class="face-action-sub">{{ challengeHint }}</div>
      <div class="face-action-sub">点击“{{ copy.start }}”后开始动作，慢一点，持续约 3 秒。</div>
      <div class="face-motion-tip">
        动作大一点：转头到能看到侧脸，抬头或低头要明显；脸保持在画面中央即可，
        不用把整个头塞进框里。
      </div>
      <div v-if="lightStatus === 'dark'" class="face-light-tip">
        光线偏暗：请面向光源或开灯，避免背光和屏幕反光。
      </div>
      <div v-else-if="lightStatus === 'bright'" class="face-light-tip">
        光线过亮：请稍微避开直射光源或调整角度。
      </div>
    </div>

    <div class="face-actions">
      <button v-if="!cameraReady" class="btn btn-primary" type="button"
              :disabled="busy" @click="startCamera">
        {{ cameraError ? '重试摄像头' : '允许使用摄像头' }}
      </button>
      <button v-else-if="!capturing" class="btn btn-primary" type="button"
              :disabled="busy || lightStatus !== 'ok'" @click="startCapture">
        {{ busy ? '上传中…' : copy.start }}
      </button>
      <button class="btn btn-ghost" type="button" :disabled="busy" @click="$emit('cancel')">
        取消
      </button>
    </div>
    <p class="face-note">{{ copy.note }}</p>
    <p v-if="isWeChat" class="face-warn">
      微信内置浏览器可能限制连续摄像头采集，建议点右上角在系统浏览器（Safari / Chrome）打开。
    </p>
  </div>
</template>

<script setup>
import { computed, onUnmounted, ref, watch } from 'vue'

const props = defineProps({
  session: { type: Object, required: true },
  busy: { type: Boolean, default: false },
  // State-machine input: "probe" | "enroll" | "verify".
  purpose: { type: String, default: 'verify' }
})
const emit = defineEmits(['captured', 'cancel'])

const copy = computed(() => {
  const flows = {
    probe: {
      openHint: '点击下方按钮开启摄像头',
      guideHint: '脸放在画面中央即可',
      progress: '检测中',
      start: '开始检测',
      note: '不会保存照片。'
    },
    enroll: {
      openHint: '点击下方按钮开启摄像头',
      guideHint: '脸放在画面中央即可',
      progress: '采集中',
      start: '开始登记',
      note: '不会保存照片，之后可随时解除。'
    },
    verify: {
      openHint: '点击下方按钮开启摄像头',
      guideHint: '脸放在画面中央即可',
      progress: '验证中',
      start: '开始验证',
      note: '验证失败可改用邮箱验证码。'
    }
  }
  return flows[props.purpose] || flows.verify
})

const isWeChat = /MicroMessenger/i.test(navigator.userAgent)
const video = ref(null)
const stream = ref(null)
const cameraReady = ref(false)
const cameraError = ref('')
const capturing = ref(false)
const phase = ref('ready')
const countdown = ref(3)
const capturedCount = ref(0)
// Capture 8 frames at 400 ms intervals (about 3.2 s) so the whole head
// movement is covered, not just the final pose.
const frameTarget = ref(8)
const frameIntervalMs = 400
const lightStatus = ref('checking')
const lightValue = ref(0)
let countdownTimer = null
let lightTimer = null

const lightLabel = computed(() => {
  if (lightStatus.value === 'dark') return `光线偏暗 ${lightValue.value}%`
  if (lightStatus.value === 'bright') return `光线过亮 ${lightValue.value}%`
  if (lightStatus.value === 'ok') return '光线正常'
  return '检测光线…'
})

const challengeLabels = {
  turn_left: {
    title: '请向左慢慢转头，幅度大一点',
    hint: '从正脸开始，转到能看到侧脸，停一下再回正'
  },
  turn_right: {
    title: '请向右慢慢转头，幅度大一点',
    hint: '从正脸开始，转到能看到侧脸，停一下再回正'
  },
  look_up: {
    title: '请慢慢抬头，幅度大一点',
    hint: '下巴抬高，停一下再回正，脸不要出框'
  },
  look_down: {
    title: '请慢慢低头，幅度大一点',
    hint: '下巴收低，停一下再回正，脸不要出框'
  }
}
watch(() => props.session?.session_token, () => {
  phase.value = 'ready'
  capturing.value = false
  capturedCount.value = 0
  countdown.value = 3
})

const challengeLabel = computed(() =>
  (challengeLabels[props.session?.challenge] || { title: '请正对镜头，按提示完成动作' }).title)
const challengeHint = computed(() =>
  (challengeLabels[props.session?.challenge] || { hint: '画面里只保留你一个人，光线要充足' }).hint)

function stopLightCheck() {
  if (lightTimer) {
    clearInterval(lightTimer)
    lightTimer = null
  }
}

function stopCamera() {
  stopLightCheck()
  if (stream.value) {
    for (const track of stream.value.getTracks()) track.stop()
    stream.value = null
  }
  cameraReady.value = false
}

function checkLight() {
  const videoEl = video.value
  if (!videoEl || videoEl.readyState < 2) return
  try {
    const canvas = document.createElement('canvas')
    canvas.width = 64
    canvas.height = 48
    const ctx = canvas.getContext('2d', { willReadFrequently: true })
    ctx.drawImage(videoEl, 0, 0, 64, 48)
    const data = ctx.getImageData(16, 12, 32, 24).data
    let sum = 0
    let count = 0
    for (let i = 0; i < data.length; i += 4) {
      sum += 0.299 * data[i] + 0.587 * data[i + 1] + 0.114 * data[i + 2]
      count += 1
    }
    const average = count ? sum / count : 0
    lightValue.value = Math.round((average / 255) * 100)
    if (average < 45) lightStatus.value = 'dark'
    else if (average > 235) lightStatus.value = 'bright'
    else lightStatus.value = 'ok'
  } catch {
    // If pixel inspection is unavailable, do not block capture.
    lightStatus.value = 'ok'
  }
}

function startLightCheck() {
  stopLightCheck()
  checkLight()
  lightTimer = setInterval(checkLight, 400)
}

async function startCamera() {
  cameraError.value = ''
  if (!window.isSecureContext) {
    cameraError.value = '当前页面无法使用摄像头，请通过 HTTPS 打开'
    return
  }
  if (!navigator.mediaDevices?.getUserMedia) {
    cameraError.value = '当前浏览器不支持摄像头，请换用系统浏览器'
    return
  }
  try {
    stream.value = await navigator.mediaDevices.getUserMedia({
      video: {
        facingMode: 'user',
        width: { ideal: 1280 },
        height: { ideal: 720 }
      },
      audio: false
    })
    if (video.value) {
      video.value.srcObject = stream.value
      await video.value.play()
    }
    cameraReady.value = true
    try {
      const track = stream.value.getVideoTracks()[0]
      if (track?.applyConstraints) {
        await track.applyConstraints({ advanced: [{ exposureMode: 'continuous' }] })
        const caps = track.getCapabilities?.()
        const maxExposure = caps?.exposureCompensation?.max
        if (typeof maxExposure === 'number' && maxExposure > 0) {
          await track.applyConstraints({
            advanced: [{ exposureCompensation: Math.min(maxExposure, 1) }]
          })
        }
      }
    } catch {
      // Optional camera control; safe to ignore.
    }
    startLightCheck()
  } catch (error) {
    cameraError.value =
      error?.name === 'NotAllowedError'
        ? '未获得摄像头权限，请在浏览器设置中允许'
        : '无法打开摄像头，请检查设备'
  }
}

async function drawFrame() {
  const videoEl = video.value
  const canvas = document.createElement('canvas')
  canvas.width = 640
  canvas.height = 480
  const ctx = canvas.getContext('2d')
  const vw = videoEl.videoWidth
  const vh = videoEl.videoHeight
  const targetRatio = 4 / 3
  let sw = vw
  let sh = vh
  let sx = 0
  let sy = 0
  if (vw / vh > targetRatio) {
    sw = vh * targetRatio
    sx = (vw - sw) / 2
  } else {
    sh = vw / targetRatio
    sy = (vh - sh) / 2
  }
  ctx.drawImage(videoEl, sx, sy, sw, sh, 0, 0, 640, 480)

  let brightness = 0
  let sharpness = 0
  try {
    const metric = document.createElement('canvas')
    metric.width = 160
    metric.height = 120
    const mctx = metric.getContext('2d', { willReadFrequently: true })
    mctx.drawImage(canvas, 0, 0, 160, 120)
    const pixels = mctx.getImageData(0, 0, 160, 120).data
    const gray = new Float32Array(160 * 120)
    let bsum = 0
    let bcount = 0
    for (let y = 0; y < 120; y += 1) {
      for (let x = 0; x < 160; x += 1) {
        const i = (y * 160 + x) * 4
        const luma = 0.299 * pixels[i] + 0.587 * pixels[i + 1] + 0.114 * pixels[i + 2]
        gray[y * 160 + x] = luma
        if (x >= 40 && x < 120 && y >= 30 && y < 90) {
          bsum += luma
          bcount += 1
        }
      }
    }
    brightness = bcount ? bsum / bcount : 0
    let sum = 0
    let sumSq = 0
    let count = 0
    for (let y = 1; y < 119; y += 1) {
      for (let x = 1; x < 159; x += 1) {
        const i = y * 160 + x
        const lap = 4 * gray[i] - gray[i - 1] - gray[i + 1] - gray[i - 160] - gray[i + 160]
        sum += lap
        sumSq += lap * lap
        count += 1
      }
    }
    const mean = count ? sum / count : 0
    sharpness = count ? sumSq / count - mean * mean : 0
  } catch {
    brightness = 128
    sharpness = 100
  }
  // JPEG encodes far faster than PNG on mobile WebViews (WeChat), so the
  // progress counter advances at the intended 400 ms cadence instead of
  // waiting hundreds of ms per frame for PNG compression.
  const blob = await new Promise((resolve) => canvas.toBlob(resolve, 'image/jpeg', 0.9))
  return blob ? { blob, brightness, sharpness } : null
}

async function captureFrames() {
  capturing.value = true
  phase.value = 'capture'
  const shots = []
  for (let i = 0; i < frameTarget.value; i += 1) {
    const shot = await drawFrame()
    if (shot) {
      shot.index = i
      shots.push(shot)
      capturedCount.value = shots.length
    }
    if (i < frameTarget.value - 1) {
      await new Promise((resolve) => setTimeout(resolve, frameIntervalMs))
    }
  }
  capturing.value = false
  phase.value = 'done'
  // Drop clearly dark/overexposed/blurred frames. Send 6 frames: keep both
  // ends of the movement for motion range, and the 4 sharpest middle frames.
  // Sending all 8 including two blurred transition frames makes upstream
  // report identity_mismatch (identity_min/outliers) even when the median is
  // high, so limit to the 6 cleanest frames that still span the action.
  const good = shots.filter((shot) =>
    shot.brightness >= 45 && shot.brightness <= 235 && shot.sharpness >= 8)
  let chosen = good.slice(0, 6)
  if (good.length > 6) {
    const first = good[0]
    const last = good[good.length - 1]
    const middle = good.slice(1, -1)
      .sort((a, b) => b.sharpness - a.sharpness)
      .slice(0, 4)
    chosen = [first, ...middle, last].sort((a, b) => a.index - b.index)
  }
  emit('captured', chosen.length >= 6 ? chosen.map((shot) => shot.blob) : [])
}

function startCapture() {
  if (!cameraReady.value || capturing.value || props.busy) return
  if (lightStatus.value !== 'ok') return
  capturedCount.value = 0
  phase.value = 'capture'
  // Start immediately: the click is the cue. A 3-second countdown made many
  // users perform the head movement before frames were actually captured.
  captureFrames()
}

onUnmounted(() => {
  if (countdownTimer) clearInterval(countdownTimer)
  stopLightCheck()
  stopCamera()
})
</script>

<style scoped>
.face-capture {
  display: grid;
  grid-template-columns: minmax(0, 440px) minmax(0, 1fr);
  grid-template-areas:
    "stage instruction"
    "stage actions"
    "stage note"
    "stage warn";
  column-gap: 24px;
  row-gap: 12px;
  align-items: start;
  width: 100%;
  max-width: 820px;
  margin: 0 auto;
}
.face-stage {
  grid-area: stage;
  position: relative;
  width: 100%;
  max-width: 440px;
  aspect-ratio: 4 / 3;
  overflow: hidden;
  border: 1px solid var(--line);
  background: #111;
}
.face-instruction { grid-area: instruction; }
.face-actions { grid-area: actions; }
.face-note { grid-area: note; }
.face-warn { grid-area: warn; }
.face-video {
  width: 100%;
  height: 100%;
  object-fit: cover;
  display: block;
  transform: none;
}
.face-guide {
  position: absolute;
  left: 50%;
  top: 50%;
  width: 66%;
  height: 90%;
  transform: translate(-50%, -50%);
  border: 2px solid rgba(255, 255, 255, .88);
  border-radius: 50%;
  box-shadow: 0 0 0 9999px rgba(17, 17, 17, .14);
  pointer-events: none;
}
.face-guide::before,
.face-guide::after {
  content: "";
  position: absolute;
  left: 50%;
  width: 18px;
  height: 2px;
  transform: translateX(-50%);
  background: rgba(255, 255, 255, .92);
}
.face-guide::before { top: -1px; }
.face-guide::after { bottom: -1px; }
.face-light {
  position: absolute;
  top: 10px;
  left: 10px;
  padding: 4px 8px;
  background: rgba(17, 17, 17, .58);
  color: #fff;
  font-size: 11px;
  line-height: 1.4;
  pointer-events: none;
}
.face-light.dark { color: #ffd9a8; }
.face-light.bright { color: #fff3b0; }
.face-light.ok { color: #b9f0cc; }
.face-light-tip {
  margin-top: 8px;
  font-size: 11.5px;
  line-height: 1.7;
  color: var(--danger);
}
.face-motion-tip {
  margin-top: 8px;
  padding: 7px 9px;
  border-left: 2px solid var(--ink);
  background: var(--fill);
  font-size: 11.5px;
  line-height: 1.7;
  color: var(--ink-soft);
}
.face-guide-hint {
  position: absolute;
  left: 50%;
  bottom: 12px;
  transform: translateX(-50%);
  padding: 4px 9px;
  background: rgba(17, 17, 17, .55);
  color: rgba(255, 255, 255, .92);
  font-size: 11px;
  line-height: 1.4;
  white-space: nowrap;
  pointer-events: none;
}
.face-stage-note {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  color: #f2f2f2;
  font-size: 13px;
  background: rgba(17, 17, 17, .72);
}
.face-stage-error { color: #ffd9d4; }
.face-count {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 64px;
  font-weight: 300;
  color: #fff;
  text-shadow: 0 2px 18px rgba(0, 0, 0, .45);
}
.face-progress {
  position: absolute;
  left: 12px;
  right: 12px;
  bottom: 12px;
  display: flex;
  justify-content: space-between;
  padding: 7px 10px;
  font-size: 12px;
  color: #fff;
  background: rgba(17, 17, 17, .62);
}
.face-instruction {
  padding: 12px 14px;
  border-left: 2px solid var(--ink);
  background: var(--fill);
}
.face-action { font-size: 14px; color: var(--ink); }
.face-action-sub { margin-top: 4px; font-size: 12px; color: var(--ink-faint); }
.face-actions { display: flex; flex-wrap: wrap; gap: 8px; }
.face-note { margin: 0; font-size: 11.5px; line-height: 1.7; color: var(--ink-faint); }
.face-warn {
  margin: 0;
  padding: 8px 10px;
  border-left: 2px solid var(--warn);
  background: var(--warn-soft);
  font-size: 11.5px;
  line-height: 1.7;
  color: var(--warn);
}
@media (max-width: 720px) {
  .face-capture {
    display: grid;
    grid-template-columns: 1fr;
    grid-template-areas:
      "stage"
      "instruction"
      "actions"
      "note"
      "warn";
    max-width: 520px;
    row-gap: 10px;
  }
  .face-stage {
    max-width: 100%;
    max-height: 38vh;
  }
}
</style>
