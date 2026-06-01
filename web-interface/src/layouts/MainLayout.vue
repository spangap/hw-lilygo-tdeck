<template>
  <q-layout v-if="authChecked" view="hHh Lpr fFf" class="main-layout">
    <q-header class="bg-dark text-white no-shadow app-header">
      <MenuBar />
    </q-header>

    <q-drawer
      :model-value="panelOpen"
      side="left"
      :width="drawerWidth"
      behavior="desktop"
      :overlay="false"
      bordered
      class="settings-drawer bg-grey-10 text-white"
      :breakpoint="0"
      @update:model-value="onDrawerToggle"
    >
      <SettingsPanel />
    </q-drawer>

    <q-page-container class="main-page-container">
      <UsableArea>
        <router-view />
        <TerminalWindow
          :visible="cliVisible"
          title="CLI"
          dc-label="cli:1"
          config-prefix="cli"
          @update:visible="v => cliVisible = v"
        />
        <LogWindow
          :visible="logVisible"
          title="System Log"
          @update:visible="v => logVisible = v"
        />
        <NodesWindow
          :visible="nodesVisible"
          title="Reticulum Nodes"
          @update:visible="v => nodesVisible = v"
        />
        <MapWindow
          :visible="mapVisible"
          title="Reticulum Map"
          @update:visible="v => mapVisible = v"
        />
        <MessagesWindow
          :visible="messagesVisible"
          title="LXMF"
          @update:visible="v => messagesVisible = v"
        />
        <AnnouncesWindow
          :visible="announcesVisible"
          title="Announces"
          @update:visible="v => announcesVisible = v"
        />
        <NomadWindow
          :visible="nomadVisible"
          title="Nomad Browser"
          @update:visible="v => nomadVisible = v"
        />
      </UsableArea>
    </q-page-container>
  </q-layout>
</template>

<script setup lang="ts">
import { computed, onMounted, ref, onUnmounted } from 'vue'
import { useQuasar } from 'quasar'
import { useRouter } from 'vue-router'
import { useMenuStore } from 'spangap-browser/stores/menu'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { checkAuth, isAdminUnset } from 'spangap-browser/lib/auth'
import { getSession, type SessionState } from 'spangap-browser/lib/webrtc-session'
import MenuBar from 'spangap-browser/components/MenuBar.vue'
import SettingsPanel from 'spangap-browser/components/SettingsPanel.vue'
import TerminalWindow from 'spangap-browser/components/TerminalWindow.vue'
import LogWindow from 'spangap-browser/components/LogWindow.vue'
import UsableArea from 'spangap-browser/components/UsableArea.vue'
import MapWindow from 'rns/panels/MapWindow.vue'
import NodesWindow from 'rns/panels/NodesWindow.vue'
import AnnouncesWindow from 'lxmf/panels/AnnouncesWindow.vue'
import MessagesWindow from 'lxmf/panels/MessagesWindow.vue'
import NomadWindow from 'nomad/panels/NomadWindow.vue'
import { cliVisible, logVisible } from 'spangap-browser/modules/advanced'
import { mapVisible, nodesVisible } from 'rns/modules/rnsd'
import { messagesVisible, announcesVisible } from 'lxmf/modules/lxmf'
import { nomadVisible } from 'nomad/modules/nomad'
import { startLogStream, installConsoleHooks } from 'spangap-browser/stores/log'

const $q = useQuasar()
const router = useRouter()
const menuStore = useMenuStore()
const device = useDeviceStore()
const authChecked = ref(false)

const panelOpen = computed(() => menuStore.activePanel !== null)

const drawerWidth = computed(() =>
  Math.min(420, Math.max(280, Math.floor($q.screen.width * 0.38))),
)

function onDrawerToggle(val: boolean) {
  if (!val) menuStore.closePanel()
}

const session = getSession()
const sessionState = ref<SessionState>(session.state)
let sessionUnsub: (() => void) | null = null

onMounted(async () => {
  try {
    const unset = await isAdminUnset()
    if (unset) { router.replace('/setup'); return }
    const auth = await checkAuth()
    if (auth.enabled && !auth.realm) { router.replace('/login'); return }
  } catch { /* proceed */ }
  authChecked.value = true
  sessionUnsub = session.onStateChange((s) => { sessionState.value = s })
  device.connect()
  startLogStream()
  installConsoleHooks()
})

onUnmounted(() => {
  if (sessionUnsub) { sessionUnsub(); sessionUnsub = null }
})
</script>

<style scoped>
.main-layout {
  height: 100vh;
  overflow: hidden;
}
.main-page-container {
  height: 100%;
  overflow: hidden;
  display: flex;
  flex-direction: column;
}
.app-header {
  box-shadow: none !important;
  border-bottom: 2px solid rgba(255, 255, 255, 0.12);
  background-image: linear-gradient(
    180deg,
    rgba(255, 255, 255, 0.055) 0%,
    rgba(255, 255, 255, 0) 42%
  );
}
</style>
