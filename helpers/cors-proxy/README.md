# Nextube CORS Proxy (Cloudflare Worker)

Because the main GitHub Release download servers (`github.com`) do not return CORS headers (`Access-Control-Allow-Origin: *`) during asset download redirects, browser-driven JavaScript running on your local network cannot fetch release ZIP packages directly. 

This folder contains a lightweight, **zero-dependency Cloudflare Worker** that resolves CORS limitations securely and efficiently.

---

## Security & Whitelisting

Unlike generic, public CORS proxies that are vulnerable to bandwidth abuse and open-relay attacks, this worker implements **strict target-domain verification**:
* It **only** proxies requests directed to official, public GitHub endpoints required for Nextube updates:
  * `github.com`
  * `api.github.com`
  * `raw.githubusercontent.com`
  * `objects.githubusercontent.com`
* Any attempt to use this worker to proxy requests to any other domain will be rejected with an immediate `403 Forbidden` response.

---

## ⚡ 2-Minute Free Deployment Guide

You can deploy this proxy completely for free on your own Cloudflare account (up to 100,000 requests per day) without installing any CLI tools:

1. **Sign Up / Log In**:
   Go to [dash.cloudflare.com](https://dash.cloudflare.com/) and log in (or create a free account).
2. **Create a Worker**:
   * Navigate to **Workers & Pages** -> **Overview** in the left sidebar.
   * Click **Create Application** (or **Create Worker**).
   * Name your worker (e.g., `nextube-updater-proxy`) and click **Deploy**.
3. **Insert Code**:
   * On your new Worker's dashboard, click **Edit Code** (or **Quick Edit**).
   * Delete any default code in the editor.
   * Copy the entire contents of [cors-proxy-worker.js](cors-proxy-worker.js) and paste it into the editor.
4. **Save and Deploy**:
   * Click **Save and Deploy** (top right).
5. **Get your URL**:
   * Your proxy is now live! Cloudflare will provide a public URL like:
     `https://nextube-updater-proxy.<your-subdomain>.workers.dev`

---

## ⚙️ How to use in Nextube-Remaster

Once your proxy is deployed, open [index.html](../../data/web/index.html), locate the `CORS_PROXY_URL` constant near the top of the update script, and change it to point to your new worker:

```javascript
const CORS_PROXY_URL = 'https://nextube-updater-proxy.<your-subdomain>.workers.dev/?url=';
```

Assemble your Web UI zip and apply it to your Nextube clock. The updater will now securely route all update downloads through your own isolated infrastructure!
