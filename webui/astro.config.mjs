import { defineConfig } from 'astro/config';

// The built files are copied into sd-card/html/ of the firmware, therefore the
// assets live next to the classic pages at the web root of the device.
export default defineConfig({
  site: 'http://aioe.local',
  base: '/',
  build: {
    assets: '_astro',
    format: 'file',
  },
  devToolbar: { enabled: false },
});
