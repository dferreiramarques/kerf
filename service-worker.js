// Kerf Service Worker (anteriormente Spliiice)
// Enables offline functionality and caching for PWA

const CACHE_NAME = 'kerf-v6';
const urlsToCache = [
  './kerf.html',
  './manifest.json',
  './vendor/jszip.min.js'
];

// Nunca guardar em cache instaladores/arquivos grandes, nem a roadmap (tem de estar sempre atual)
// nem as imagens da página (index.html)
function isNeverCached(url) {
  return url.pathname.includes('/downloads/') ||
    url.pathname.includes('/images/') ||
    url.hostname === 'roadmaps.monco.io' ||
    /\.(exe|msi|zip)$/i.test(url.pathname);
}

// App shell: network-first para que novas versões cheguem logo
function isAppShell(request, url) {
  if (request.mode === 'navigate') return true;
  if (url.origin !== self.location.origin) return false;
  return /\/(kerf\.html|index\.html|manifest\.json)$/.test(url.pathname) ||
    url.pathname === '/' ||
    /\/vendor\/[^/]+\.js$/.test(url.pathname);
}

function putInCache(request, response) {
  if (!response || response.status !== 200 || response.type === 'error') return;
  const responseToCache = response.clone();
  caches.open(CACHE_NAME).then((cache) => {
    cache.put(request, responseToCache);
  });
}

function offlineResponse() {
  return new Response('Offline - Please check your connection', {
    status: 503,
    statusText: 'Service Unavailable',
    headers: new Headers({
      'Content-Type': 'text/plain'
    })
  });
}

// Install event - cache essential files
self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => {
      return cache.addAll(urlsToCache).catch(() => {
        // If cache fails, just proceed
        console.warn('Service Worker: Could not cache all files');
      });
    })
  );
  self.skipWaiting();
});

// Activate event - clean up old caches
self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((cacheNames) => {
      return Promise.all(
        cacheNames.map((cacheName) => {
          if (cacheName !== CACHE_NAME) {
            return caches.delete(cacheName);
          }
        })
      );
    })
  );
  self.clients.claim();
});

self.addEventListener('fetch', (event) => {
  // Only handle GET requests
  if (event.request.method !== 'GET') {
    return;
  }

  const url = new URL(event.request.url);

  // Downloads (.exe/.msi/.zip): deixar passar direto para a rede, sem cache
  if (isNeverCached(url)) {
    return;
  }

  // App shell: network-first, fallback para cache quando offline
  if (isAppShell(event.request, url)) {
    event.respondWith(
      fetch(event.request).then((response) => {
        putInCache(event.request, response);
        return response;
      }).catch(() => {
        return caches.match(event.request).then((cachedResponse) => {
          if (cachedResponse) return cachedResponse;
          if (event.request.mode === 'navigate') {
            return caches.match('./kerf.html').then((shell) => shell || offlineResponse());
          }
          return offlineResponse();
        });
      })
    );
    return;
  }

  // Resto (ex: fonts): serve from cache, fallback to network
  event.respondWith(
    caches.match(event.request).then((response) => {
      if (response) {
        return response;
      }
      return fetch(event.request).then((response) => {
        putInCache(event.request, response);
        return response;
      }).catch(() => offlineResponse());
    })
  );
});
