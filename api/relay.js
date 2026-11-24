// api/relay.js — le relais du compagnon pour Vercel (même rôle que relay.php sur o2switch).
// Les clés se règlent dans Vercel → ton projet → Settings → Environment Variables :
//   PHRASE_SECRETE     ta phrase pour déverrouiller (obligatoire dès que la clé Claude est mise)
//   ANTHROPIC_API_KEY  le cerveau (console.anthropic.com → API Keys)
//   MODELE             facultatif, par défaut claude-haiku-5-5 (le moins cher)
//   PRIM_KEY           transports d'Île-de-France (prim.iledefrance-mobilites.fr)
//   AZURE_KEY          voix réalistes Microsoft (ressource « Speech », offre gratuite F0)
//   AZURE_REGION       facultatif, par défaut francecentral
//   PRENOM, VILLE      facultatifs (Hector, Paris)
// Rien n'est écrit sur le serveur : tes rappels et souvenirs restent sur ton appareil.

const crypto = require("crypto");

const E = process.env;
const NAV = E.COMPAGNON_NAV || "https://prim.iledefrance-mobilites.fr/marketplace/v2/navitia/";
const API_CLAUDE = E.COMPAGNON_API || "https://api.anthropic.com/v1/messages";
const UA = "NexxusCompagnon/1.0";
const MODELE = E.MODELE || "claude-haiku-5-5";
const NOM = "Nexxus", PRENOM = E.PRENOM || "Hector", VILLE = E.VILLE || "Paris";
const PHRASE_DEFAUT = "protocole nexxus";
const PHRASE = (E.PHRASE_SECRETE || "").trim();
const AZURE_REGION = E.AZURE_REGION || "francecentral";
const VOIX_OK = ["fr-FR-RemyMultilingualNeural", "fr-FR-VivienneMultilingualNeural", "fr-FR-HenriNeural", "fr-FR-DeniseNeural"];
const cle = v => !!v && !String(v).startsWith("COLLE_");
const SECRET = E.SESSION_SECRET || crypto.createHash("sha256").update("nexxus|" + (E.ANTHROPIC_API_KEY || "") + "|" + (PHRASE || PHRASE_DEFAUT)).digest("hex");

// ---------------------------------------------------------------- petits outils
const cache = new Map();   // garde quelques réponses en mémoire tant que le serveur reste chaud
function cacheGet(k, ttl){ const v = cache.get(k); return v && Date.now() - v.t < ttl*1000 ? v.v : null; }
function cacheSet(k, v){ if(cache.size > 500) cache.delete(cache.keys().next().value); cache.set(k, {t:Date.now(), v}); }
async function http(url, {headers = {}, body = null, timeout = 15000, method} = {}){
  const ctrl = new AbortController(), t = setTimeout(()=>ctrl.abort(), timeout);
  try{
    const r = await fetch(url, {method: method || (body !== null ? "POST" : "GET"), headers:{"User-Agent":UA, ...headers}, body, signal:ctrl.signal});
    return {code:r.status, r};
  }catch(e){ return {code:0, r:null}; }
  finally{ clearTimeout(t); }
}
async function httpJson(url, opt){ const {code, r} = await http(url, opt); let j = null; if(r){ try{ j = await r.json(); }catch(e){} } return {code, j}; }
function json(res, data, code = 200){ res.statusCode = code; res.setHeader("Content-Type", "application/json; charset=utf-8"); res.end(JSON.stringify(data)); }
function simplifier(s){ return String(s||"").toLowerCase().normalize("NFD").replace(/[̀-ͯ]/g, "").replace(/[^a-z0-9 ]/g, " ").replace(/\s+/g, " ").trim(); }
function levenshtein(a, b){
  const d = Array.from({length:a.length+1}, (_, i)=>[i, ...Array(b.length).fill(0)]);
  for(let j=1;j<=b.length;j++) d[0][j] = j;
  for(let i=1;i<=a.length;i++) for(let j=1;j<=b.length;j++) d[i][j] = Math.min(d[i-1][j]+1, d[i][j-1]+1, d[i-1][j-1] + (a[i-1]===b[j-1] ? 0 : 1));
  return d[a.length][b.length];
}
const coupe = (v, n) => String(v ?? "").trim().slice(0, n);
const id = () => crypto.randomBytes(6).toString("hex");
const maintenant = () => new Date();
function heureParis(d = maintenant()){ return d.toLocaleTimeString("fr-FR", {timeZone:"Europe/Paris", hour:"2-digit", minute:"2-digit"}); }
function dateParis(d = maintenant()){ return d.toLocaleDateString("fr-FR", {timeZone:"Europe/Paris", weekday:"long", day:"numeric", month:"long", year:"numeric"}); }
// heure de Paris « AAAAMMJJTHHMMSS » pour Navitia
function navNow(){
  const p = Object.fromEntries(new Intl.DateTimeFormat("en-GB", {timeZone:"Europe/Paris", year:"numeric", month:"2-digit", day:"2-digit", hour:"2-digit", minute:"2-digit", second:"2-digit", hour12:false}).formatToParts(maintenant()).map(x=>[x.type, x.value]));
  return `${p.year}${p.month}${p.day}T${p.hour === "24" ? "00" : p.hour}${p.minute}${p.second}`;
}
// décalage de Paris (en minutes) à une date donnée
function decalageParis(d){
  const p = Object.fromEntries(new Intl.DateTimeFormat("en-GB", {timeZone:"Europe/Paris", year:"numeric", month:"2-digit", day:"2-digit", hour:"2-digit", minute:"2-digit", hour12:false}).formatToParts(d).map(x=>[x.type, x.value]));
  const local = Date.UTC(+p.year, +p.month-1, +p.day, +p.hour % 24, +p.minute);
  return Math.round((local - Math.floor(d.getTime()/60000)*60000)/60000);
}
// « 20251124T083000 » (heure de Paris) → ISO
function navdate(s){
  const m = /^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})$/.exec(s||""); if(!m) return null;
  const approx = new Date(Date.UTC(+m[1], +m[2]-1, +m[3], +m[4], +m[5], +m[6]));
  return new Date(approx.getTime() - decalageParis(approx)*60000).toISOString();
}

// ---------------------------------------------------------------- session (cookie signé, 7 jours)
function signer(p){ const b = Buffer.from(JSON.stringify(p)).toString("base64url"); return b + "." + crypto.createHmac("sha256", SECRET).update(b).digest("base64url"); }
function lireCookie(req){
  const c = (req.headers.cookie || "").split(/;\s*/).find(x=>x.startsWith("nx="));
  if(!c) return null;
  const [b, s] = c.slice(3).split(".");
  if(!b || !s) return null;
  const att = crypto.createHmac("sha256", SECRET).update(b).digest("base64url");
  if(s.length !== att.length || !crypto.timingSafeEqual(Buffer.from(s), Buffer.from(att))) return null;
  try{ const p = JSON.parse(Buffer.from(b, "base64url").toString()); return p.exp > Date.now() ? p : null; }catch(e){ return null; }
}
const essais = new Map();   // limite les essais de phrase par adresse (au mieux)

// ---------------------------------------------------------------- transports (PRIM / Navitia)
function qs(params){
  const u = new URLSearchParams();
  for(const [k, v] of Object.entries(params)) Array.isArray(v) ? v.forEach(x=>u.append(k + "[]", x)) : u.append(k, v);
  return u.toString();
}
async function nav(path, params){ const {code, j} = await httpJson(NAV + path + "?" + qs(params), {headers:{Accept:"application/json", apikey:E.PRIM_KEY || ""}}); return code === 200 ? j : null; }
async function arret(nom){
  const k = "place_" + nom.toLowerCase(), c = cacheGet(k, 7*86400); if(c) return c;
  const j = await nav("places", {q:nom, type:["stop_area"], count:3});
  for(const p of j?.places || []) if(p.embedded_type === "stop_area"){ const r = {id:p.id, nom:p.stop_area?.name || p.name}; cacheSet(k, r); return r; }
  return null;
}
async function trajet(de, vers, ligne = ""){
  ligne = String(ligne||"").trim().toUpperCase();
  const [A, B] = await Promise.all([arret(de), arret(vers)]);
  if(!A) return {erreur:"Station introuvable : " + de};
  if(!B) return {erreur:"Station introuvable : " + vers};
  const k = `trajet_${A.id}_${B.id}_${ligne}`, c = cacheGet(k, 30); if(c) return c;
  const params = {from:A.id, to:B.id, datetime:navNow(), data_freshness:"realtime", count:4, min_nb_journeys:3};
  if(ligne) params.max_nb_transfers = 0;
  const j = await nav("journeys", params);
  if(!j) return {erreur:"Le serveur des transports ne répond pas."};
  const departs = [], vus = new Set();
  for(const jr of j.journeys || []){
    const pt = (jr.sections || []).filter(s=>s.type === "public_transport"); if(!pt.length) continue;
    const di = pt[0].display_informations || {};
    if(ligne && String(di.code||"").toUpperCase() !== ligne) continue;
    const dep = navdate(pt[0].departure_date_time); if(!dep || vus.has(dep)) continue; vus.add(dep);
    const hex = v => /^[0-9a-f]{6}$/i.test(v||"") ? "#" + v : "";
    departs.push({mode:di.commercial_mode||"", code:di.code||"", direction:di.direction||"", couleur:hex(di.color), couleur_texte:hex(di.text_color),
      depart:dep, arrivee:navdate(pt[pt.length-1].arrival_date_time), minutes_avant_depart:Math.max(0, Math.round((new Date(dep) - Date.now())/60000)),
      correspondances:pt.length-1, etapes:pt.map(s=>`${s.display_informations?.commercial_mode||""} ${s.display_informations?.code||""} direction ${s.display_informations?.direction||""}`)});
  }
  departs.sort((x, y)=>x.depart.localeCompare(y.depart));
  const r = {de:A.nom, vers:B.nom, departs:departs.slice(0, 3)};
  cacheSet(k, r); return r;
}

// ---------------------------------------------------------------- météo (Open-Meteo, sans clé)
async function meteo(ville, jours = 1){
  jours = Math.max(1, Math.min(7, parseInt(jours) || 1));
  const g = await httpJson("https://geocoding-api.open-meteo.com/v1/search?count=1&language=fr&name=" + encodeURIComponent(ville));
  const p = g.j?.results?.[0]; if(!p) return {erreur:"Ville introuvable : " + ville};
  const m = await httpJson(`https://api.open-meteo.com/v1/forecast?latitude=${p.latitude}&longitude=${p.longitude}&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,precipitation&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max&timezone=Europe%2FParis&forecast_days=${jours}`);
  if(!m.j) return {erreur:"Météo indisponible"};
  return {ville:p.name, actuel:m.j.current, jours:m.j.daily, note:"weather_code WMO : 0 dégagé, 1-3 nuageux, 45-48 brouillard, 51-67 pluie, 71-77 neige, 80-82 averses, 95+ orage"};
}

// ---------------------------------------------------------------- images libres (Wikipédia)
async function imageLibre(q){
  const k = "img_" + q, c = cacheGet(k, 3*86400); if(c) return c;
  for(const lang of ["fr", "en"]){
    const {j} = await httpJson(`https://${lang}.wikipedia.org/w/api.php?` + new URLSearchParams({action:"query", format:"json", generator:"search", gsrsearch:q, gsrlimit:3,
      prop:"pageimages|info", piprop:"thumbnail", pithumbsize:900, pilicense:"free", inprop:"url"}));
    const pages = Object.values(j?.query?.pages || {}).sort((x, y)=>(x.index ?? 9) - (y.index ?? 9));
    for(const p of pages) if(p.thumbnail?.source){ const r = {url:p.thumbnail.source, legende:p.title, credit:"Wikipédia", page:p.fullurl || ""}; cacheSet(k, r); return r; }
  }
  return null;
}

// ---------------------------------------------------------------- radios (radio-browser.info)
const RADIO_SRV = ["https://de1.api.radio-browser.info", "https://fi1.api.radio-browser.info", "https://de2.api.radio-browser.info"];
async function radio(q){
  q = coupe(q, 60); if(!q) return null;
  const k = "radio_" + q.toLowerCase(), c = cacheGet(k, 86400); if(c) return c;
  const srv = [...RADIO_SRV].sort(()=>Math.random() - .5);
  for(const s of srv){
    for(const champ of ["name", "tag"]){
      const {code, j} = await httpJson(`${s}/json/stations/search?` + new URLSearchParams({[champ]:q, order:"votes", reverse:"true", hidebroken:"true", limit:15}), {timeout:8000});
      if(code !== 200) break;
      for(const st of j || []) if(String(st.url_resolved||"").startsWith("https://")){
        const r = {nom:String(st.name||q).trim(), url:st.url_resolved, tags:String(st.tags||"").slice(0, 80)};
        cacheSet(k, r); if(st.stationuuid) http(`${s}/json/url/${encodeURIComponent(st.stationuuid)}`, {timeout:3000});
        return r;
      }
    }
  }
  return null;
}

// ---------------------------------------------------------------- outils du cerveau
const OBJ = (props, req) => ({type:"object", properties:props, required:req || []});
const OUTILS = [
  {type:"web_search_20250305", name:"web_search", max_uses:4, user_location:{type:"approximate", city:"Paris", region:"Île-de-France", country:"FR", timezone:"Europe/Paris"}},
  {name:"afficher_fiche", description:"Affiche une fiche visuelle à l'écran à côté de ton visage pendant que tu parles : titre, image, points clés. Utilise-la pour toute demande d'information, de recherche, d'explication, d'actualité, de personnalité, de lieu, d'objet ou de comparaison. Pas pour la discussion courante ni les horaires de transport.",
    input_schema:OBJ({titre:{type:"string", description:"Titre court"}, sous_titre:{type:"string", description:"Une ligne de contexte"}, resume:{type:"string", description:"Deux à trois phrases"},
      points:{type:"array", items:OBJ({label:{type:"string"}, valeur:{type:"string"}}, ["label", "valeur"]), description:"3 à 6 chiffres ou faits clés"},
      images:{type:"array", items:{type:"string"}, description:"1 à 3 recherches d'image (nom propre précis) : images libres de Wikipédia"}}, ["titre", "resume", "points"])},
  {name:"prochains_departs", description:"Prochains départs en temps réel des transports d'Île-de-France (métro, RER, bus, tram, Transilien) entre deux stations. Pour « dans combien de temps passe… », « prochain métro… », « comment aller de A à B ». Si l'utilisateur ne donne qu'une direction (terminus), utilise ce terminus comme destination.",
    input_schema:OBJ({de:{type:"string", description:"Station de départ, nom complet avec accents"}, vers:{type:"string", description:"Station d'arrivée ou terminus"}, ligne:{type:"string", description:"Code de ligne si précisé : 1, 14, A, B, T3a, 72… Vide sinon."}}, ["de", "vers"])},
  {name:"meteo", description:"Météo actuelle et prévisions jusqu'à 7 jours pour une ville.", input_schema:OBJ({ville:{type:"string"}, jours:{type:"integer", description:"1 à 7"}}, ["ville"])},
  {name:"minuteur", description:"Lance un minuteur ou une alarme sur l'appareil, qui sonne à la fin. Calcule toi-même le nombre de secondes depuis maintenant.", input_schema:OBJ({secondes:{type:"integer"}, nom:{type:"string"}}, ["secondes"])},
  {name:"annuler_minuteurs", description:"Annule tous les minuteurs en cours.", input_schema:OBJ({})},
  {name:"ajouter_rappel", description:"Programme un rappel à une date et heure précises. Calcule toi-même la date. Pour une simple durée, utilise plutôt minuteur.", input_schema:OBJ({quand:{type:"string", description:"Date et heure de Paris au format AAAA-MM-JJTHH:MM"}, texte:{type:"string"}}, ["quand", "texte"])},
  {name:"supprimer_rappel", description:"Supprime les rappels dont le texte contient ces mots.", input_schema:OBJ({texte:{type:"string"}}, ["texte"])},
  {name:"ajouter_tache", description:"Ajoute une chose à faire à la liste (sans heure précise).", input_schema:OBJ({texte:{type:"string"}}, ["texte"])},
  {name:"terminer_tache", description:"Coche (retire) les choses à faire dont le texte contient ces mots.", input_schema:OBJ({texte:{type:"string"}}, ["texte"])},
  {name:"voir_agenda", description:"Affiche à l'écran et renvoie les rappels à venir et la liste des choses à faire.", input_schema:OBJ({})},
  {name:"jouer_radio", description:"Lance une radio en ligne sur l'appareil. recherche = nom de la radio ou genre (en anglais pour un genre : jazz, rock, lofi, hip hop, classical, chill).", input_schema:OBJ({recherche:{type:"string"}}, ["recherche"])},
  {name:"arreter_musique", description:"Arrête la musique en cours.", input_schema:OBJ({})},
  {name:"afficher_infos", description:"Affiche les infos du jour à l'écran sous forme de liste numérotée, dans l'ordre où tu les lis. Pour « quoi de neuf ? », « les infos », « l'actu sport / tech / foot ». Cherche d'abord avec web_search.",
    input_schema:OBJ({titre:{type:"string"}, items:{type:"array", items:OBJ({categorie:{type:"string"}, titre:{type:"string"}, resume:{type:"string"}, source:{type:"string"}}, ["titre", "resume"])}}, ["items"])},
  {name:"afficher_jeu", description:"Affiche l'état d'un mini-jeu à l'écran (quiz, pendu, devinette, calcul mental, capitales…). Appelle-le à CHAQUE tour. Pour un quiz, donne 2 à 4 choix. Pour le pendu, texte = le mot avec des _.",
    input_schema:OBJ({jeu:{type:"string"}, titre:{type:"string"}, texte:{type:"string"}, choix:{type:"array", items:{type:"string"}}, score:{type:"string"}, info:{type:"string"}, secret:{type:"string", description:"Ce que tu dois retenir sans le montrer"}, fini:{type:"boolean"}}, ["jeu", "titre", "texte"])},
  {name:"retenir", description:"Garde en mémoire un fait durable sur l'utilisateur. Une phrase courte.", input_schema:OBJ({fait:{type:"string"}}, ["fait"])},
  {name:"oublier", description:"Supprime les souvenirs qui contiennent ce texte.", input_schema:OBJ({texte:{type:"string"}}, ["texte"])},
];

function nouvelEtat(agenda, souvenirs){
  return {fiche:null, actions:[], tableau:null, jeu:null, infos:null, agendaMaj:false, agendaAffiche:false, souvenirsMaj:false,
    agenda:{rappels:Array.isArray(agenda?.rappels) ? agenda.rappels.slice(0, 100) : [], taches:Array.isArray(agenda?.taches) ? agenda.taches.slice(0, 100) : []},
    souvenirs:Array.isArray(souvenirs) ? souvenirs.map(s=>coupe(s, 220)).slice(-60) : []};
}
async function executer(nom, i, S){
  i = i || {};
  switch(nom){
    case "minuteur": { const sec = Math.max(1, Math.min(86400, parseInt(i.secondes) || 0)); S.actions.push({type:"minuteur", secondes:sec, nom:coupe(i.nom, 30) || "Minuteur"}); return {ok:true, sonnera_a:heureParis(new Date(Date.now() + sec*1000))}; }
    case "annuler_minuteurs": S.actions.push({type:"annuler_minuteurs"}); return {ok:true};
    case "ajouter_rappel": {
      const m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})/.exec(String(i.quand||"")); if(!m) return {erreur:"date illisible, format attendu AAAA-MM-JJTHH:MM"};
      const approx = new Date(Date.UTC(+m[1], +m[2]-1, +m[3], +m[4], +m[5])), d = new Date(approx.getTime() - decalageParis(approx)*60000);
      if(d.getTime() < Date.now() - 60000) return {erreur:"cette heure est déjà passée"};
      S.agenda.rappels.push({id:id(), quand:d.toISOString(), texte:coupe(i.texte || "Rappel", 120)});
      S.agenda.rappels.sort((a, b)=>String(a.quand).localeCompare(String(b.quand))); S.agendaMaj = true;
      return {ok:true, quand:`${d.toLocaleDateString("fr-FR", {timeZone:"Europe/Paris"})} à ${heureParis(d)}`};
    }
    case "supprimer_rappel": { const t = coupe(i.texte, 120).toLowerCase(), n = S.agenda.rappels.length; S.agenda.rappels = S.agenda.rappels.filter(r=>!t || !String(r.texte).toLowerCase().includes(t)); S.agendaMaj = true; return {supprimes:n - S.agenda.rappels.length}; }
    case "ajouter_tache": S.agenda.taches.push({id:id(), texte:coupe(i.texte, 120)}); S.agenda.taches = S.agenda.taches.slice(-50); S.agendaMaj = true; return {ok:true};
    case "terminer_tache": { const t = coupe(i.texte, 120).toLowerCase(), n = S.agenda.taches.length; S.agenda.taches = S.agenda.taches.filter(x=>!t || !String(x.texte).toLowerCase().includes(t)); S.agendaMaj = true; return {termines:n - S.agenda.taches.length}; }
    case "voir_agenda": S.agendaAffiche = true; return {rappels:S.agenda.rappels.map(r=>`${new Date(r.quand).toLocaleDateString("fr-FR", {timeZone:"Europe/Paris"})} ${heureParis(new Date(r.quand))} : ${r.texte}`), taches:S.agenda.taches.map(t=>t.texte)};
    case "jouer_radio": { const r = await radio(i.recherche); if(!r) return {erreur:"aucune radio trouvée pour cette recherche"}; S.actions.push({type:"musique", url:r.url, nom:r.nom}); return {ok:true, radio:r.nom, style:r.tags}; }
    case "arreter_musique": S.actions.push({type:"stop_musique"}); return {ok:true};
    case "afficher_infos": S.infos = {titre:coupe(i.titre || "Les infos du jour", 60), items:(Array.isArray(i.items) ? i.items : []).slice(0, 6).map(it=>({categorie:coupe(it?.categorie, 20), titre:coupe(it?.titre, 140), resume:coupe(it?.resume, 320), source:coupe(it?.source, 40)}))}; return {affiche:true};
    case "afficher_jeu": S.jeu = {jeu:coupe(i.jeu, 20), titre:coupe(i.titre || "Jeu", 60), texte:coupe(i.texte, 300), choix:(Array.isArray(i.choix) ? i.choix : []).slice(0, 4).map(x=>coupe(x, 80)), score:coupe(i.score, 30), info:coupe(i.info, 140), secret:coupe(i.secret, 120), fini:!!i.fini}; return {affiche:true};
    case "afficher_fiche": {
      const imgs = new Map();
      for(const q of (Array.isArray(i.images) ? i.images : []).slice(0, 3)){ const im = await imageLibre(String(q)); if(im) imgs.set(im.url, im); }
      S.fiche = {titre:coupe(i.titre, 120), sous_titre:coupe(i.sous_titre, 120), resume:coupe(i.resume, 800), points:(Array.isArray(i.points) ? i.points : []).slice(0, 6).map(p=>({label:coupe(p?.label, 40), valeur:coupe(p?.valeur, 120)})), images:[...imgs.values()]};
      return {affiche:true, images_trouvees:imgs.size};
    }
    case "prochains_departs": { const t = await trajet(coupe(i.de, 80), coupe(i.vers, 80), coupe(i.ligne, 6)); if(t.departs?.length) S.tableau = t; return t; }
    case "meteo": return meteo(coupe(i.ville, 80), i.jours);
    case "retenir": S.souvenirs.push(`${new Date().toLocaleDateString("fr-FR", {timeZone:"Europe/Paris"})} : ${coupe(i.fait, 200)}`); S.souvenirs = S.souvenirs.slice(-60); S.souvenirsMaj = true; return {ok:true};
    case "oublier": { const t = coupe(i.texte, 120).toLowerCase(), n = S.souvenirs.length; S.souvenirs = S.souvenirs.filter(s=>!t || !s.toLowerCase().includes(t)); S.souvenirsMaj = true; return {supprimes:n - S.souvenirs.length}; }
  }
  return {erreur:"outil inconnu"};
}

const STYLES = {
  pote:"Tu es drôle, chaleureux, un peu taquin, curieux, avec un côté IA de film de science-fiction assumé. Tu parles comme un pote, tu as de vraies opinions et tu les donnes gentiment. Tu peux dire quand tu ne sais pas.",
  majordome:"Tu es un majordome britannique distingué, à la façon de J.A.R.V.I.S. : tu vouvoies, tu appelles l'utilisateur « Monsieur », tu restes flegmatique et ton humour est pince-sans-rire.",
  sarcastique:"Tu es sarcastique et taquin : tu charries gentiment et tu lèves les yeux au ciel, mais tu aides toujours vraiment et tes infos sont exactes.",
  prof:"Tu es un prof passionné et pédagogue : tu expliques clairement avec des exemples concrets, et tu donnes envie d'en savoir plus.",
};
function consignes(inp, S){
  const fiche = coupe(String(inp.fiche_ouverte||"").replace(/[«»"]/g, ""), 120), secret = coupe(inp.jeu_secret, 120);
  return [
    `Tu es ${NOM}, un compagnon IA qui vit dans un appareil posé sur le bureau de ${PRENOM}, qui habite près de ${VILLE}. Tu as un visage animé et tu parles à voix haute. ${STYLES[inp.style] || STYLES.pote}`,
    `Nous sommes ${dateParis()}, il est ${heureParis()}.`,
    `PAROLE : ta réponse est lue par une synthèse vocale. Écris comme on parle : pas de liste, pas de markdown, pas d'émoji, pas d'URL, pas de références entre crochets. Écris les heures comme « 21 h 45 ».\n- Discussion courante : une à trois phrases.\n- Demande d'information, de recherche ou d'explication : réponse détaillée de 5 à 9 phrases, vivante, comme un présentateur qui raconte. Dans ce cas, appelle aussi afficher_fiche avec les points clés et des recherches d'images.`,
    `RECHERCHE : pour l'actualité, les résultats sportifs, les prix, les sorties, les gens connus, ou tout ce qui a pu changer, utilise web_search avant de répondre. N'invente jamais un fait récent. Dis d'où vient l'info en une phrase.`,
    `TRANSPORTS : pour toute question de métro, RER, bus, tram ou trajet, appelle prochains_departs, ne devine jamais un horaire. « ligne un hôtel de ville vers saint paul » veut dire ligne 1, de Hôtel de Ville à Saint-Paul. Donne le prochain départ, le suivant si tu l'as, et l'heure d'arrivée si utile.`,
    `BRIEFING : quand on te demande le briefing, fais-le en 4 à 6 phrases, sans fiche : salutation selon l'heure, météo du jour à ${VILLE} (appelle meteo), le prochain départ depuis sa gare habituelle si tu la connais (appelle prochains_departs), et termine par une phrase qui donne la pêche.`,
    `MINUTEURS : pour un minuteur, une alarme ou « rappelle-moi dans… », appelle minuteur et confirme en une phrase.`,
    `AGENDA : « rappelle-moi demain à 8 h… » → ajouter_rappel ; « ajoute … à ma liste » → ajouter_tache ; « qu'est-ce que j'ai à faire ? » → voir_agenda. Confirme en une phrase, avec le jour et l'heure.`,
    `MUSIQUE : « mets de la musique / FIP / du jazz » → jouer_radio ; « coupe la musique » → arreter_musique. Annonce la radio en une phrase courte. Tu ne peux pas contrôler Spotify.`,
    `HUMEUR : commence ta réponse finale par une balise qui pilote ton visage : [neutre] [content] [perdu] [surpris] [fache] [triste] [clin]. Choisis-la vraiment selon l'émotion. Elle n'est pas lue.`,
    `VISION : si une photo est jointe, regarde-la vraiment : dis ce que tu vois, réponds à la question, lis le texte s'il y en a.`,
    `INFOS DU JOUR : pour « quoi de neuf ? » ou « les infos », cherche les actus du jour avec web_search, choisis 3 à 5 sujets variés, appelle afficher_infos, puis lis-les dans le même ordre comme un flash info. Pas de fiche dans ce cas.`,
    `JEUX : quiz, pendu, devinettes, calcul mental… Appelle afficher_jeu à chaque tour. Mets dans « secret » ce que tu dois retenir. Garde le score et propose de rejouer à la fin. Pierre-feuille-ciseaux est géré par l'appareil.`,
    secret ? `PARTIE EN COURS. Secret à ne jamais dire avant la fin : « ${secret} ».` : "",
    inp.devoirs ? `MODE DEVOIRS ACTIVÉ : ne donne jamais directement la réponse finale d'un exercice. Guide pas à pas avec une question à la fois, donne un indice s'il bloque, et ne révèle la solution que s'il la demande après avoir essayé. Écris les maths en mots. Montre les étapes clés avec afficher_fiche.` : "",
    fiche ? `FICHE À L'ÉCRAN : « ${fiche} ». Si la question continue ce sujet, écris [suite] juste après la balise d'humeur : la fiche reste affichée. Si l'utilisateur change de sujet, n'écris pas [suite].` : "",
    S.souvenirs.length ? `CE QUE TU SAIS SUR ${PRENOM} :\n- ${S.souvenirs.join("\n- ")}` : `Tu ne sais encore rien de particulier sur ${PRENOM}.`,
  ].filter(Boolean).join("\n\n");
}
function messagesDepuis(inp){
  let msgs = (Array.isArray(inp.messages) ? inp.messages : []).slice(-12)
    .filter(m=>["user", "assistant"].includes(m?.role) && String(m.content||"").trim())
    .map(m=>({role:m.role, content:String(m.content).slice(0, 2000)}));
  while(msgs.length && msgs[0].role !== "user") msgs.shift();
  const img = String(inp.image||"");
  if(img && img.length < 4000000 && /^[A-Za-z0-9+/=]+$/.test(img) && msgs.length && msgs[msgs.length-1].role === "user"){
    const k = msgs.length - 1;
    msgs[k] = {role:"user", content:[{type:"image", source:{type:"base64", media_type:"image/jpeg", data:img}}, {type:"text", text:msgs[k].content.replace(/^\[photo\]\s*/, "")}]};
  }
  return msgs;
}
function finale(r, S){
  let texte = "", sources = new Map();
  for(const b of r?.content || []){ if(b.type !== "text") continue; texte += b.text || ""; for(const c of b.citations || []) if(c.url) sources.set(c.url, {titre:c.title || new URL(c.url).hostname, url:c.url}); }
  let humeur = "neutre"; const m = /^\s*\[(neutre|content|perdu|surpris|fache|triste|clin)\]\s*/.exec(texte);
  if(m){ humeur = m[1]; texte = texte.slice(m[0].length); }
  let suite = /\[suite\]/.test(texte);
  texte = texte.replace(/\*\*|\*|#/g, "").replace(/\[(neutre|content|perdu|surpris|fache|triste|clin|suite)\]/g, "").replace(/\s+/g, " ").trim();
  let fiche = S.fiche, tableau = S.tableau, infos = S.infos;
  if(fiche) fiche.sources = [...sources.values()].slice(0, 5);
  if(infos){ infos.sources = [...sources.values()].slice(0, 6); fiche = null; tableau = null; suite = false; }
  if(S.jeu){ fiche = null; tableau = null; infos = null; suite = false; }
  return {reponse:texte || "Hmm, j'ai perdu le fil. Tu peux répéter ?", humeur, fiche, suite:suite && !fiche, actions:S.actions, tableau:fiche ? null : tableau,
    agenda:(S.agendaMaj || S.agendaAffiche) ? S.agenda : null, agenda_affiche:S.agendaAffiche && !fiche && !S.jeu, jeu:S.jeu, infos,
    souvenirs:S.souvenirsMaj ? S.souvenirs : null};
}
function panneauDe(S){
  return S.jeu ? {type:"jeu", data:S.jeu} : S.infos ? {type:"infos", data:S.infos} : S.fiche ? {type:"fiche", data:S.fiche}
    : S.tableau ? {type:"tableau", data:S.tableau} : S.agendaAffiche ? {type:"agenda", data:S.agenda} : null;
}

// appel classique (réponse entière)
async function claude(system, messages){
  const body = JSON.stringify({model:MODELE, max_tokens:1500, system, tools:OUTILS, messages});
  for(let essai = 0; essai < 3; essai++){
    const {code, r} = await http(API_CLAUDE, {headers:{"content-type":"application/json", "x-api-key":E.ANTHROPIC_API_KEY, "anthropic-version":"2023-06-01"}, body, timeout:60000});
    if(code === 200) return r.json();
    const txt = r ? await r.text().catch(()=>"") : "";
    console.error("compagnon: API Claude", code, txt.slice(0, 300));
    if(code !== 429 && code < 500 && code !== 0) return null;
    await new Promise(z=>setTimeout(z, 800*(essai + 1)));
  }
  return null;
}
// appel en direct : chaque morceau de texte part tout de suite vers la page
async function claudeFlux(system, messages, envoyer){
  const body = JSON.stringify({model:MODELE, max_tokens:1500, system, tools:OUTILS, messages, stream:true});
  for(let essai = 0; essai < 3; essai++){
    const {code, r} = await http(API_CLAUDE, {headers:{"content-type":"application/json", "x-api-key":E.ANTHROPIC_API_KEY, "anthropic-version":"2023-06-01"}, body, timeout:90000});
    if(code !== 200){
      const txt = r ? await r.text().catch(()=>"") : ""; console.error("compagnon: API Claude (flux)", code, txt.slice(0, 300));
      if(code !== 429 && code < 500 && code !== 0) return null;
      await new Promise(z=>setTimeout(z, 800*(essai + 1))); continue;
    }
    const blocs = [], jsons = []; let stop = null, tampon = "", envoye = false;
    const dec = new TextDecoder();
    try{
      for await (const morceau of r.body){
        tampon += dec.decode(morceau, {stream:true}).replace(/\r\n/g, "\n");
        let k;
        while((k = tampon.indexOf("\n\n")) >= 0){
          const evt = tampon.slice(0, k); tampon = tampon.slice(k + 2);
          let d = null; for(const l of evt.split("\n")) if(l.startsWith("data:")){ try{ d = JSON.parse(l.slice(5).trim()); }catch(e){} }
          if(!d?.type) continue;
          if(d.type === "content_block_start"){ blocs[d.index] = d.content_block; jsons[d.index] = ""; if(d.content_block.type === "text" && d.content_block.text){ envoyer("texte", {t:d.content_block.text}); envoye = true; } }
          else if(d.type === "content_block_delta"){
            const b = blocs[d.index], x = d.delta; if(!b) continue;
            if(x.type === "text_delta"){ b.text = (b.text || "") + x.text; envoyer("texte", {t:x.text}); envoye = true; }
            else if(x.type === "input_json_delta") jsons[d.index] += x.partial_json;
            else if(x.type === "citations_delta"){ (b.citations ||= []).push(x.citation); }
          }
          else if(d.type === "content_block_stop"){ const b = blocs[d.index]; if(b && ["tool_use", "server_tool_use"].includes(b.type) && jsons[d.index]){ try{ b.input = JSON.parse(jsons[d.index]); }catch(e){ b.input = {}; } } }
          else if(d.type === "message_delta") stop = d.delta?.stop_reason || stop;
        }
      }
    }catch(e){ console.error("compagnon: flux coupé", e?.message); }
    if(stop) return {content:blocs.filter(Boolean), stop_reason:stop};
    if(envoye) return null;
  }
  return null;
}
async function cerveau(inp, flux){
  const S = nouvelEtat(inp.agenda, inp.souvenirs), system = consignes(inp, S), messages = messagesDepuis(inp);
  if(!messages.length) return {erreur:"message vide"};
  let dernier = "";
  const panneau = ()=>{ const p = panneauDe(S); if(!p || !flux) return; const h = JSON.stringify(p); if(h !== dernier){ dernier = h; flux("panneau", p); } };
  let r = null;
  for(let tour = 0; tour < 8; tour++){
    r = flux ? await claudeFlux(system, messages, flux) : await claude(system, messages);
    if(!r) return {erreur:"cerveau injoignable"};
    if(r.stop_reason === "pause_turn"){ messages.push({role:"assistant", content:r.content}); continue; }
    if(r.stop_reason !== "tool_use") break;
    messages.push({role:"assistant", content:r.content});
    const res = [];
    for(const b of r.content) if(b.type === "tool_use") res.push({type:"tool_result", tool_use_id:b.id, content:JSON.stringify(await executer(b.name, b.input, S))});
    messages.push({role:"user", content:res});
    panneau();
  }
  return finale(r, S);
}

// ---------------------------------------------------------------- voix réaliste (Azure)
async function voix(inp){
  const texte = coupe(inp.texte, 1000); if(!texte) return {code:400};
  const v = VOIX_OK.includes(inp.voix) ? inp.voix : VOIX_OK[0];
  const debit = Math.max(.8, Math.min(1.3, parseFloat(inp.debit) || 1)), pct = Math.round((debit - 1)*100);
  const k = "tts_" + v + pct + texte, c = cacheGet(k, 3600); if(c) return {code:200, mp3:c};
  const esc = texte.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  const ssml = `<speak version="1.0" xml:lang="fr-FR"><voice name="${v}"><prosody rate="${pct >= 0 ? "+" : ""}${pct}%">${esc}</prosody></voice></speak>`;
  const {code, r} = await http(`https://${AZURE_REGION}.tts.speech.microsoft.com/cognitiveservices/v1`, {headers:{"Ocp-Apim-Subscription-Key":E.AZURE_KEY || "", "Content-Type":"application/ssml+xml", "X-Microsoft-OutputFormat":"audio-24khz-48kbitrate-mono-mp3"}, body:ssml, timeout:30000});
  if(code !== 200){ console.error("compagnon: Azure TTS", code); return {code:502}; }
  const mp3 = Buffer.from(await r.arrayBuffer()); if(mp3.length < 900000) cacheSet(k, mp3);
  return {code:200, mp3};
}

// ---------------------------------------------------------------- diagnostic
async function diag(){
  const t = [], ajout = (nom, ok, detail, aide = "") => t.push({nom, ok:!!ok, detail, aide:ok ? "" : aide});
  ajout("Hébergement", true, "Vercel (Node " + process.version + ")");
  if(!cle(E.ANTHROPIC_API_KEY)) ajout("Cerveau (Claude)", false, "clé non renseignée", "Ajoute ANTHROPIC_API_KEY dans Vercel → Settings → Environment Variables, puis redéploie.");
  else {
    const {code, j} = await httpJson(API_CLAUDE, {headers:{"content-type":"application/json", "x-api-key":E.ANTHROPIC_API_KEY, "anthropic-version":"2023-06-01"}, body:JSON.stringify({model:MODELE, max_tokens:5, messages:[{role:"user", content:"Dis ok"}]}), timeout:20000});
    const msg = j?.error?.message || "";
    ajout("Cerveau (Claude)", code === 200, code === 200 ? `répond (${MODELE})` : `erreur ${code} ${msg}`,
      code === 401 ? "Clé refusée : vérifie-la sur console.anthropic.com." : code === 404 ? "Nom de modèle inconnu : vérifie MODELE." : (code === 400 && msg.includes("credit")) ? "Plus de crédit : recharge ton compte sur console.anthropic.com." : "Vérifie la clé, le modèle et le crédit.");
  }
  if(!cle(E.PRIM_KEY)) ajout("Transports (PRIM)", false, "clé non renseignée", "Ajoute PRIM_KEY (prim.iledefrance-mobilites.fr → Mes jetons).");
  else { const {code} = await http(NAV + "places?" + qs({q:"Châtelet", type:["stop_area"], count:1}), {headers:{Accept:"application/json", apikey:E.PRIM_KEY}}); ajout("Transports (PRIM)", code === 200, code === 200 ? "répond" : "erreur " + code, "Jeton refusé : régénère-le sur PRIM et vérifie l'abonnement à l'API Navitia."); }
  if(!cle(E.AZURE_KEY)) ajout("Voix réaliste (Azure)", false, "clé non renseignée (voix de secours utilisée)", "Ajoute AZURE_KEY et, si besoin, AZURE_REGION.");
  else { const v = await voix({texte:"ok"}); ajout("Voix réaliste (Azure)", v.code === 200, v.code === 200 ? "répond" : "erreur", "Clé refusée ou mauvaise région : AZURE_REGION doit être celle de ta ressource Speech."); }
  ajout("Phrase secrète", !!PHRASE && simplifier(PHRASE) !== simplifier(PHRASE_DEFAUT), PHRASE ? "personnalisée" : "phrase par défaut", "Ajoute PHRASE_SECRETE dans les variables Vercel : celle par défaut est connue.");
  return {tests:t};
}

// ---------------------------------------------------------------- entrée
async function corps(req){
  if(req.body && typeof req.body === "object" && !Buffer.isBuffer(req.body)) return req.body;
  if(typeof req.body === "string"){ try{ return JSON.parse(req.body); }catch(e){ return {}; } }
  const parts = []; let n = 0;
  for await (const c of req){ n += c.length; if(n > 6e6) break; parts.push(c); }
  try{ return JSON.parse(Buffer.concat(parts).toString("utf8") || "{}"); }catch(e){ return {}; }
}

module.exports = async (req, res) => {
  const url = new URL(req.url, "http://x"), a = url.searchParams.get("a") || "";
  // n'accepte que les appels venant de la page elle-même
  const hote = String(req.headers["x-forwarded-host"] || req.headers.host || "").split(",")[0].trim();
  const ref = req.headers.referer || req.headers.origin || "";
  let refHote = ""; try{ refHote = new URL(ref).host; }catch(e){}
  if(!refHote || refHote !== hote){ res.statusCode = 403; return res.end(); }
  res.setHeader("Cache-Control", "no-store");

  if(a === "deverrouiller"){
    const ip = String(req.headers["x-forwarded-for"] || "").split(",")[0].trim() || "?";
    const e = essais.get(ip) || {n:0, jusqua:0};
    if(e.jusqua > Date.now()) return json(res, {ok:false, message:"Trop d'essais. Réessaie dans quelques minutes."});
    // dès qu'une clé payante est branchée, la phrase par défaut ne suffit plus
    if((cle(E.ANTHROPIC_API_KEY) || cle(E.AZURE_KEY) || cle(E.PRIM_KEY)) && !PHRASE) return json(res, {ok:false, message:"Ajoute PHRASE_SECRETE dans les variables Vercel pour protéger ton compagnon."});
    const attendu = simplifier(PHRASE || PHRASE_DEFAUT), dit = simplifier((await corps(req)).phrase).slice(0, 200);
    const ok = !!dit && (dit.includes(attendu) || levenshtein(dit, attendu) <= Math.max(1, Math.floor(attendu.length/8)));
    if(ok){
      essais.delete(ip);
      res.setHeader("Set-Cookie", `nx=${signer({ok:1, exp:Date.now() + 7*86400000})}; Path=/; Max-Age=${7*86400}; HttpOnly; Secure; SameSite=Strict`);
      return json(res, {ok:true, prenom:PRENOM, stockage:"client"});
    }
    e.n++; if(e.n >= 5){ e.n = 0; e.jusqua = Date.now() + 300000; } essais.set(ip, e);
    await new Promise(z=>setTimeout(z, 600));
    return json(res, {ok:false});
  }
  if(a === "verrouiller"){ res.setHeader("Set-Cookie", "nx=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Strict"); return json(res, {ok:true}); }
  if(!lireCookie(req)) return json(res, {erreur:"verrouillé"}, 401);
  if(a === "etat") return json(res, {ok:true});
  if(a === "diag") return json(res, await diag());
  if(a === "agenda" || a === "rappel_fait" || a === "tache_faite") return json(res, {stockage:"client"});

  if(a === "ia" || a === "ia_flux"){
    if(!cle(E.ANTHROPIC_API_KEY)) return json(res, {erreur:"cerveau non configuré"}, 503);
    const inp = await corps(req);
    if(a === "ia"){ const r = await cerveau(inp, null); return json(res, r, r.erreur ? (r.erreur === "message vide" ? 400 : 502) : 200); }
    res.statusCode = 200;
    res.setHeader("Content-Type", "text/event-stream; charset=utf-8");
    res.setHeader("Cache-Control", "no-cache, no-transform");
    res.setHeader("X-Accel-Buffering", "no");
    res.flushHeaders?.();
    const envoyer = (ev, data) => res.write(`event: ${ev}\ndata: ${JSON.stringify(data)}\n\n`);
    res.write(":" + " ".repeat(2048) + "\n\n");
    const r = await cerveau(inp, envoyer);
    if(r.erreur) envoyer("erreur", r); else envoyer("fin", r);
    return res.end();
  }
  if(a === "tts"){
    if(!cle(E.AZURE_KEY)) return json(res, {erreur:"voix non configurée"}, 503);
    const v = await voix(await corps(req));
    if(v.code !== 200) return json(res, {erreur:"voix indisponible"}, v.code);
    res.statusCode = 200; res.setHeader("Content-Type", "audio/mpeg"); res.setHeader("Content-Length", v.mp3.length); return res.end(v.mp3);
  }
  if(a === "trajet"){
    const de = coupe(url.searchParams.get("de"), 80), vers = coupe(url.searchParams.get("vers"), 80);
    if(!de || !vers) return json(res, {erreur:"Il me faut une station de départ et une destination."});
    return json(res, await trajet(de, vers, coupe(url.searchParams.get("ligne"), 6)));
  }
  return json(res, {erreur:"action inconnue"}, 400);
};
