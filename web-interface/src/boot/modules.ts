import type { boot } from 'quasar/wrappers'
import SettingSlider from 'spangap-browser/components/SettingSlider.vue'
import SettingToggle from 'spangap-browser/components/SettingToggle.vue'
import SettingSelect from 'spangap-browser/components/SettingSelect.vue'
import SettingText from 'spangap-browser/components/SettingText.vue'
import PanelHeading from 'spangap-browser/components/PanelHeading.vue'
import { registerSystem } from 'spangap-browser/modules/system'
import { registerNetwork } from 'spangap-browser/modules/network'
import { registerAdvanced } from 'spangap-browser/modules/advanced'
import { registerUpnp } from 'upnp/modules/upnp'
import { registerWg } from 'wg/modules/wg'
import { registerSshd } from 'sshd/modules/sshd'
import { registerDuckDns } from 'duckdns/modules/duckdns'
import { registerAcme } from 'acme/modules/acme'
import { registerRnsd } from 'rns/modules/rnsd'
import { registerTcp } from 'tr-tcp/modules/tcp'
import { registerAuto } from 'tr-auto/modules/auto'
import { registerLora } from 'tr-lora/modules/lora'
import { registerEspnow } from 'tr-espnow/modules/espnow'
import { registerLxmf } from 'lxmf/modules/lxmf'
import { registerNomad } from 'nomad/modules/nomad'

export default ({ app }: Parameters<Parameters<typeof boot>[0]>[0]) => {
  app.component('SettingSlider', SettingSlider)
  app.component('SettingToggle', SettingToggle)
  app.component('SettingSelect', SettingSelect)
  app.component('SettingText', SettingText)
  app.component('PanelHeading', PanelHeading)

  registerSystem()
  registerNetwork()
  registerUpnp()
  registerWg()
  registerSshd()
  registerDuckDns()
  registerAcme()
  registerRnsd()
  registerTcp()
  registerAuto()
  registerLora()
  registerEspnow()
  registerLxmf()
  registerNomad()
  registerAdvanced()
}
