export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const targetUrl = url.searchParams.get('url');

    // Récupérer les infos de l'appelant (fournies par Cloudflare)
    const ip = request.headers.get('cf-connecting-ip') || 'Unknown IP';
    const country = request.headers.get('cf-ipcountry') || 'Unknown Country';

    if (!targetUrl) {
      console.log(`[BAD REQUEST] IP: ${ip} (${country}) - No URL parameter specified.`);
      return new Response('Missing "url" query parameter', { status: 400 });
    }

    // --- SÉCURITÉ : Liste blanche des dépôts autorisés ---
    const lowerTarget = targetUrl.toLowerCase();
    const allowedPrefixes = [
      'https://github.com/mrtoast99/nextube-remaster/',
      'https://raw.githubusercontent.com/mrtoast99/nextube-remaster/'
    ];

    const isAuthorized = allowedPrefixes.some(prefix => lowerTarget.startsWith(prefix));

    if (!isAuthorized) {
      console.warn(`[BLOCKED] IP: ${ip} (${country}) attempted to proxy unauthorized URL: ${targetUrl}`);
      return new Response(
        'Forbidden: This CORS proxy is restricted to authorized Nextube-Remaster repositories only.',
        { status: 403 }
      );
    }

    console.log(`[AUTHORIZED] IP: ${ip} (${country}) downloading: ${targetUrl}`);

    // --- Gérer la requête de pré-vérification CORS (Preflight) ---
    if (request.method === 'OPTIONS') {
      return new Response(null, {
        headers: {
          'Access-Control-Allow-Origin': '*',
          'Access-Control-Allow-Methods': 'GET, OPTIONS',
          'Access-Control-Allow-Headers': '*',
        }
      });
    }

    try {
      const response = await fetch(targetUrl, {
        headers: { 'User-Agent': request.headers.get('User-Agent') || 'Nextube-Updater' }
      });

      const newHeaders = new Headers(response.headers);
      newHeaders.set('Access-Control-Allow-Origin', '*');
      newHeaders.set('Access-Control-Expose-Headers', 'Content-Length');

      return new Response(response.body, {
        status: response.status,
        statusText: response.statusText,
        headers: newHeaders
      });
    } catch (e) {
      console.error(`[ERROR] Failed to fetch target URL. Message: ${e.message}`);
      return new Response(`Proxy Error: ${e.message}`, { status: 500 });
    }
  }
};