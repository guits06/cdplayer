// Self-destroying service worker - clears all caches and unregisters
self.addEventListener('install', (e) => {
  self.skipWaiting();
});

self.addEventListener('activate', (e) => {
  e.waitUntil(
    caches.keys().then((keys) => {
      return Promise.all(keys.map((key) => caches.delete(key)));
    }).then(() => {
      return self.clients.claim();
    })
  );
});

// Never cache anything - always go to network
self.addEventListener('fetch', (e) => {
  // Don't intercept - let browser handle normally
  return;
});
