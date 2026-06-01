const PAIR_TTL_SECONDS = 10 * 60;
const TOKEN_TTL_SECONDS = 2 * 60;
const LICHESS_AUTHORIZE_URL = "https://lichess.org/oauth";
const LICHESS_TOKEN_URL = "https://lichess.org/api/token";
const LICHESS_ACCOUNT_URL = "https://lichess.org/api/account";

export default {
  async fetch(request, env) {
    try {
      return await handleRequest(request, env);
    } catch (error) {
      return json({ error: "internal_error", detail: String(error?.message || error) }, 500);
    }
  },
};

async function handleRequest(request, env) {
  const url = new URL(request.url);

  if (request.method === "POST" && url.pathname === "/api/pair/start") {
    return startPairing(env);
  }

  if (request.method === "GET" && url.pathname === "/api/pair/status") {
    return pairStatus(url, env);
  }

  if (request.method === "GET" && url.pathname.startsWith("/pair/")) {
    return pairPage(url, env);
  }

  if (request.method === "GET" && url.pathname === "/oauth/start") {
    return oauthStart(url, env);
  }

  if (request.method === "GET" && url.pathname === "/oauth/callback") {
    return oauthCallback(url, env);
  }

  return new Response("Not found", { status: 404 });
}

async function startPairing(env) {
  const deviceId = randomId(24);
  const pairCode = `${randomChars(4)}-${randomChars(4)}`;
  const state = randomId(24);
  const codeVerifier = randomId(64);

  await env.PAIRINGS.put(
    sessionKey(deviceId),
    JSON.stringify({ pairCode, state, codeVerifier, createdAt: Date.now(), status: "pending" }),
    { expirationTtl: PAIR_TTL_SECONDS },
  );
  await env.PAIRINGS.put(codeKey(pairCode), deviceId, { expirationTtl: PAIR_TTL_SECONDS });
  await env.PAIRINGS.put(stateKey(state), deviceId, { expirationTtl: PAIR_TTL_SECONDS });

  return json({
    deviceId,
    pairCode,
    pairUrl: `${baseUrl(env)}/pair/${encodeURIComponent(pairCode)}`,
    expiresIn: PAIR_TTL_SECONDS,
  });
}

async function pairStatus(url, env) {
  const deviceId = url.searchParams.get("deviceId") || "";
  const session = await readSession(env, deviceId);
  if (!session) {
    return json({ status: "expired" });
  }

  if (session.status !== "linked") {
    return json({ status: "pending" });
  }

  await cleanupSession(env, deviceId, session);
  return json({ status: "linked", username: session.username, accessToken: session.accessToken });
}

async function pairPage(url, env) {
  const pairCode = decodeURIComponent(url.pathname.slice("/pair/".length)).toUpperCase();
  const deviceId = await env.PAIRINGS.get(codeKey(pairCode));
  const session = await readSession(env, deviceId);

  if (!session || session.pairCode !== pairCode) {
    return html(page("Expired Pairing", `<p>This pairing code is expired or invalid.</p>`), 404);
  }

  return html(page("Connect Lichess", `
    <p class="code">${escapeHtml(pairCode)}</p>
    <p>This connects your board to Lichess with only the <code>board:play</code> permission.</p>
    <a class="button" href="/oauth/start?code=${encodeURIComponent(pairCode)}">Login with Lichess</a>
  `));
}

async function oauthStart(url, env) {
  const pairCode = (url.searchParams.get("code") || "").toUpperCase();
  const deviceId = await env.PAIRINGS.get(codeKey(pairCode));
  const session = await readSession(env, deviceId);
  if (!session || session.pairCode !== pairCode) {
    return html(page("Expired Pairing", `<p>This pairing code is expired or invalid.</p>`), 404);
  }

  const challenge = await pkceChallenge(session.codeVerifier);
  const authorize = new URL(LICHESS_AUTHORIZE_URL);
  authorize.searchParams.set("response_type", "code");
  authorize.searchParams.set("client_id", env.LICHESS_CLIENT_ID);
  authorize.searchParams.set("redirect_uri", `${baseUrl(env)}/oauth/callback`);
  authorize.searchParams.set("scope", "board:play");
  authorize.searchParams.set("state", session.state);
  authorize.searchParams.set("code_challenge", challenge);
  authorize.searchParams.set("code_challenge_method", "S256");
  return Response.redirect(authorize.toString(), 302);
}

async function oauthCallback(url, env) {
  const code = url.searchParams.get("code") || "";
  const state = url.searchParams.get("state") || "";
  const deviceId = await env.PAIRINGS.get(stateKey(state));
  const session = await readSession(env, deviceId);

  if (!code || !session || session.state !== state) {
    return html(page("Login Failed", `<p>The login session is expired or invalid.</p>`), 400);
  }

  const token = await exchangeCodeForToken(code, session.codeVerifier, env);
  const username = await loadLichessUsername(token.access_token);
  const linkedSession = {
    ...session,
    status: "linked",
    username,
    accessToken: token.access_token,
    linkedAt: Date.now(),
  };

  await env.PAIRINGS.put(sessionKey(deviceId), JSON.stringify(linkedSession), { expirationTtl: TOKEN_TTL_SECONDS });
  return html(page("Board Linked", `
    <p>Your board is linked as <strong>${escapeHtml(username)}</strong>.</p>
    <p>You can return to the chess board.</p>
  `));
}

async function exchangeCodeForToken(code, codeVerifier, env) {
  const body = new URLSearchParams();
  body.set("grant_type", "authorization_code");
  body.set("code", code);
  body.set("redirect_uri", `${baseUrl(env)}/oauth/callback`);
  body.set("client_id", env.LICHESS_CLIENT_ID);
  body.set("code_verifier", codeVerifier);
  if (env.LICHESS_CLIENT_SECRET) {
    body.set("client_secret", env.LICHESS_CLIENT_SECRET);
  }

  const response = await fetch(LICHESS_TOKEN_URL, {
    method: "POST",
    headers: { "content-type": "application/x-www-form-urlencoded" },
    body,
  });
  const jsonBody = await response.json().catch(() => ({}));
  if (!response.ok || !jsonBody.access_token) {
    throw new Error(`token_exchange_failed:${response.status}`);
  }
  return jsonBody;
}

async function loadLichessUsername(accessToken) {
  const response = await fetch(LICHESS_ACCOUNT_URL, {
    headers: { authorization: `Bearer ${accessToken}`, accept: "application/json" },
  });
  const body = await response.json().catch(() => ({}));
  if (!response.ok || !body.username) {
    throw new Error(`account_lookup_failed:${response.status}`);
  }
  return body.username;
}

async function readSession(env, deviceId) {
  if (!deviceId) {
    return null;
  }
  const raw = await env.PAIRINGS.get(sessionKey(deviceId));
  return raw ? JSON.parse(raw) : null;
}

async function cleanupSession(env, deviceId, session) {
  await Promise.all([
    env.PAIRINGS.delete(sessionKey(deviceId)),
    env.PAIRINGS.delete(codeKey(session.pairCode)),
    env.PAIRINGS.delete(stateKey(session.state)),
  ]);
}

function sessionKey(deviceId) {
  return `session:${deviceId}`;
}

function codeKey(pairCode) {
  return `code:${pairCode}`;
}

function stateKey(state) {
  return `state:${state}`;
}

function baseUrl(env) {
  return String(env.PUBLIC_BASE_URL || "").replace(/\/$/, "");
}

function randomChars(length) {
  const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  const bytes = crypto.getRandomValues(new Uint8Array(length));
  return Array.from(bytes, (byte) => alphabet[byte % alphabet.length]).join("");
}

function randomId(length) {
  const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  const bytes = crypto.getRandomValues(new Uint8Array(length));
  return Array.from(bytes, (byte) => alphabet[byte % alphabet.length]).join("");
}

async function pkceChallenge(verifier) {
  const bytes = new TextEncoder().encode(verifier);
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return base64Url(new Uint8Array(digest));
}

function base64Url(bytes) {
  let binary = "";
  for (const byte of bytes) {
    binary += String.fromCharCode(byte);
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function json(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" },
  });
}

function html(body, status = 200) {
  return new Response(body, {
    status,
    headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store" },
  });
}

function page(title, content) {
  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${escapeHtml(title)}</title>
  <style>
    body{margin:0;background:#17202a;color:#263526;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;display:grid;min-height:100vh;place-items:center}
    main{box-sizing:border-box;width:min(92vw,520px);background:#f1e1bb;border:4px solid #9fbad0;border-radius:24px;padding:28px;text-align:center;box-shadow:0 18px 40px #0005}
    h1{margin:0 0 18px;font-size:32px}.code{font-size:42px;letter-spacing:.12em;font-weight:800;margin:20px 0;color:#17202a}p{font-size:18px;line-height:1.45}.button{display:block;margin:28px auto 0;padding:16px 20px;background:#34495e;color:#f8f1dc;border-radius:14px;text-decoration:none;font-weight:800;font-size:20px}code{background:#f8fafc99;border-radius:6px;padding:2px 6px}
  </style>
</head>
<body><main><h1>${escapeHtml(title)}</h1>${content}</main></body>
</html>`;
}

function escapeHtml(value) {
  return String(value).replace(/[&<>"]/g, (char) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[char]);
}
