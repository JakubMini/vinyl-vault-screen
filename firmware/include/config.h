#pragma once

// The gateway Worker the screen talks to. Not secret: it only forwards what a linked screen may
// do. For local work, point it at `npm run dev` in gateway/ on this laptop, e.g.
//   PLATFORMIO_BUILD_FLAGS='-DGATEWAY_URL=\"http://192.168.0.159:8787\"' pio run -t upload
#ifndef GATEWAY_URL
#define GATEWAY_URL "https://vinyl-vault-screen.jakub-m-szypicyn.workers.dev"
#endif
