// Nexxus — page de réglages servie par le boîtier (http://nexxus.local)
#pragma once
#include <Arduino.h>

static const char PAGE_REGLAGES[] PROGMEM = R"HTML(<!doctype html>
<html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Komi</title>
<style>
:root{--f:#07090d;--c:#11161e;--b:#1d2530;--t:#e8edf3;--g:#8b95a3;--a:#35e0ff;--r:#ff6b6b;--v:#4ade80}
*{box-sizing:border-box}body{margin:0;background:var(--f);color:var(--t);font:15px/1.45 system-ui,-apple-system,sans-serif;padding:16px 16px 60px}
main{max-width:560px;margin:auto}h1{font-size:22px;margin:6px 0 2px;display:flex;align-items:center;gap:10px}
.yeux{display:inline-flex;gap:5px}.yeux i{width:10px;height:13px;border-radius:4px;background:var(--a);box-shadow:0 0 8px var(--a)}
.sous{color:var(--g);margin:0 0 16px}section{background:var(--c);border:1px solid var(--b);border-radius:16px;padding:16px;margin:12px 0}
h2{font-size:15px;margin:0 0 12px;letter-spacing:.02em}label{display:block;color:var(--g);font-size:13px;margin:10px 0 4px}
input,select,textarea{width:100%;background:#0b0f15;color:var(--t);border:1px solid var(--b);border-radius:10px;padding:10px 12px;font:inherit}
input[type=range]{padding:0;accent-color:var(--a)}input[type=color]{height:42px;padding:3px}
button{background:var(--a);color:#001018;border:0;border-radius:10px;padding:10px 16px;font:600 15px system-ui;cursor:pointer}
button.second{background:#1a222d;color:var(--t)}button.danger{background:#2a1416;color:var(--r)}
.ligne{display:flex;gap:8px;align-items:center}.ligne>*{flex:1}.ligne button{flex:0 0 auto}
.etat{display:grid;grid-template-columns:1fr 1fr;gap:8px}.etat div{background:#0b0f15;border-radius:10px;padding:8px 10px}
.etat b{display:block;font-size:12px;color:var(--g);font-weight:500}.ok{color:var(--v)}.ko{color:var(--r)}
.puces{display:flex;flex-wrap:wrap;gap:6px}.puces button{background:#1a222d;color:var(--t);padding:8px 12px;font-weight:500}
.res{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-top:1px solid var(--b)}
.note{color:var(--g);font-size:13px;margin:8px 0 0}#toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:#1a222d;padding:10px 16px;border-radius:12px;opacity:0;transition:.3s}
#toast.on{opacity:1}.tests div{padding:6px 0;border-top:1px solid var(--b)}progress{width:100%}
</style></head><body><main>
<h1><span class="yeux"><i></i><i></i></span><span id="titre">Komi</span></h1>
<p class="sous" id="sous">Réglages du boîtier</p>

<section><h2>État</h2><div class="etat" id="etat"></div></section>

<section><h2>Lui parler</h2>
<div class="ligne"><input id="q" placeholder="Pose-lui une question…"><button onclick="cmd('? '+q.value);q.value=''">Envoyer</button></div>
<div class="ligne" style="margin-top:8px"><input id="dire" placeholder="Lui faire dire une phrase…"><button class="second" onclick="cmd('dis '+dire.value);dire.value=''">Dire</button></div>
<label>Humeur</label><div class="puces">
<button onclick="cmd('humeur content')">😊 Content</button><button onclick="cmd('humeur surpris')">😮 Surpris</button>
<button onclick="cmd('humeur triste')">😢 Triste</button><button onclick="cmd('humeur fache')">😠 Fâché</button>
<button onclick="cmd('humeur clin')">😉 Clin d'œil</button><button onclick="cmd('humeur amour')">😍 Amour</button>
<button onclick="cmd('humeur dort')">😴 Dodo</button><button onclick="cmd('humeur neutre')">🙂 Normal</button></div>
<label>Montrer</label><div class="puces"><button onclick="cmd('carte heure')">Heure</button><button onclick="cmd('carte appareil')">Infos</button><button onclick="cmd('tais-toi')">Silence</button></div>
</section>

<section><h2>Cerveau</h2>
<label>Adresse du relais</label><input id="serveur" placeholder="https://nexxus.pm/relay.php">
<label>Code appareil (le même que $CODE_APPAREIL dans relay.php)</label>
<div class="ligne"><input id="code" placeholder="laisser vide pour ne pas changer" autocomplete="off"><button class="second" onclick="genCode()">Créer</button></div>
<label>Personnalité</label><select id="style"><option value="pote">Le pote</option><option value="majordome">Le majordome</option><option value="sarcastique">Le sarcastique</option><option value="prof">Le prof</option><option value="coach">Le coach sportif</option><option value="zen">Le zen</option><option value="pirate">Le pirate</option></select>
<p class="note">Astuce : double-clic sur le bouton BOOT pour changer de personnalité directement sur le boîtier.</p>
<div class="ligne"><div><label>Son nom</label><input id="nom"></div><div><label>Ton prénom</label><input id="prenom"></div></div>
<label>Ta ville (météo de l'écran de veille)</label><input id="ville" placeholder="Paris">
<p class="ligne" style="margin-top:14px"><button onclick="sauver(['serveur','code','style','nom','prenom','ville'])">Enregistrer</button><button class="second" onclick="tester()">Tester</button></p>
<div class="tests" id="tests"></div>
</section>

<section><h2>Voix et son</h2>
<label>Voix</label><select id="voix"><option value="fr-FR-RemyMultilingualNeural">Rémy</option><option value="fr-FR-VivienneMultilingualNeural">Vivienne</option><option value="fr-FR-HenriNeural">Henri</option><option value="fr-FR-DeniseNeural">Denise</option></select>
<label>Vitesse <span id="vdebit"></span></label><input type="range" id="debit" min="0.8" max="1.3" step="0.05">
<label>Volume <span id="vvolume"></span></label><input type="range" id="volume" min="0" max="100">
<label>Sensibilité des micros <span id="vmicro"></span></label><input type="range" id="micro" min="12" max="37">
<p class="ligne" style="margin-top:14px"><button onclick="sauver(['voix','debit','volume','micro'])">Enregistrer</button><button class="second" onclick="cmd('dis Coucou, voilà ma voix !')">Essayer</button></p>
</section>

<section><h2>Écran</h2>
<label>Forme des yeux</label><select id="forme"><option value="0">Arrondis</option><option value="1">Ronds</option><option value="2">Carrés</option><option value="3">Fins</option><option value="4">Grands</option></select>
<label>Couleurs toutes prêtes</label><div class="puces" id="palette"></div>
<div class="ligne"><div><label>Couleur des yeux</label><input type="color" id="couleur"></div><div><label>Luminosité <span id="vluminosite"></span></label><input type="range" id="luminosite" min="10" max="255"></div></div>
<label>Mise en veille après (minutes, 0 = jamais)</label><input type="number" id="veille" min="0" max="120">
<label><input type="checkbox" id="nuit" style="width:auto"> Mode nuit (il dort et l'écran baisse)</label>
<div class="ligne"><div><label>de</label><input type="number" id="nuit_debut" min="0" max="23"></div><div><label>à</label><input type="number" id="nuit_fin" min="0" max="23"></div></div>
<label><input type="checkbox" id="sous_titres" style="width:auto"> Sous-titres quand il parle</label>
<label><input type="checkbox" id="ndd" style="width:auto"> Ne pas déranger (pas de proposition ni de sonnerie de rappel ; les réveils sonnent)</label>
<div class="ligne"><div><label>de</label><input type="number" id="ndd_debut" min="0" max="23"></div><div><label>à</label><input type="number" id="ndd_fin" min="0" max="23"></div></div>
<label><input type="checkbox" id="ecole" style="width:auto"> Mode école automatique du lundi au vendredi (écran éteint, aucun son ; appui long 3 s sur BOOT pour sortir)</label>
<div class="ligne"><div><label>de</label><input type="number" id="ecole_debut" min="0" max="23"></div><div><label>à</label><input type="number" id="ecole_fin" min="0" max="23"></div></div>
<label><input type="checkbox" id="ecole_mercredi" style="width:auto"> Le mercredi, seulement le matin</label>
<label>Objectif de pas par jour</label><input type="number" id="objectif_pas" min="500" max="50000" step="500">
<p style="margin-top:14px"><button onclick="sauver(['forme','couleur','luminosite','veille','nuit','nuit_debut','nuit_fin','sous_titres','ndd','ndd_debut','ndd_fin','objectif_pas','ecole','ecole_debut','ecole_fin','ecole_mercredi'])">Enregistrer</button></p>
</section>

<section><h2>Premier essai</h2>
<p class="note" style="margin-top:0">Le mode test affiche 4 cibles, le niveau du micro et un bouton pour le son. Si le rond n'apparaît pas sous ton doigt, change le sens du toucher.</p>
<p class="ligne"><button class="second" onclick="cmd('test')">Mode test sur l'écran</button><button class="second" onclick="cmd('fermer')">Fermer</button></p>
<label>Sens du toucher</label><select id="sens_toucher" onchange="sauver(['sens_toucher'])"><option value="0">Normal</option><option value="1">Gauche/droite inversé</option><option value="2">Haut/bas inversé</option><option value="3">Les deux inversés</option></select>
</section>

<section><h2>Wi-Fi</h2>
<p class="note" style="margin-top:0">Il se connecte tout seul au premier réseau connu disponible. Ajoute ta box et le partage de connexion de ton téléphone.</p>
<div id="reseaux"></div>
<label>Ajouter un réseau</label>
<div class="ligne"><select id="ssid"><option value="">— Choisir —</option></select><button class="second" onclick="scan()">Chercher</button></div>
<input id="ssidm" placeholder="ou écris son nom" style="margin-top:8px">
<input id="mdp" type="password" placeholder="Mot de passe" style="margin-top:8px">
<p style="margin-top:12px"><button onclick="ajouterWifi()">Ajouter</button></p>
</section>

<section><h2>Wi-Fi des invités</h2>
<p class="note">Dis « montre le Wi-Fi invités » : il affiche un QR code que tes amis scannent pour se connecter. Le mot de passe reste dans le boîtier.</p>
<label>Nom du réseau</label><input id="wifi_invite_nom" maxlength="32">
<label>Mot de passe</label><input id="wifi_invite_mdp" type="password" maxlength="63" placeholder="(laisser vide pour garder l'actuel)">
<p style="margin-top:14px"><button onclick="sauver(['wifi_invite_nom','wifi_invite_mdp']);$('wifi_invite_mdp').value=''">Enregistrer</button></p>
</section>
<section><h2>Mise à jour</h2>
<p class="note" style="margin-top:0">Envoie le fichier <b>komi-maj.bin</b>. Il redémarre tout seul à la fin.</p>
<input type="file" id="fichier" accept=".bin"><progress id="prog" value="0" max="100" hidden></progress>
<p style="margin-top:12px"><button onclick="maj()">Mettre à jour</button></p>
</section>

<section><h2>Système</h2><div class="ligne"><button class="second" onclick="post('/api/redemarrer')">Redémarrer</button>
<button class="danger" onclick="if(confirm('Tout effacer (réglages, Wi-Fi) ?'))post('/api/usine')">Tout réinitialiser</button></div></section>
</main><div id="toast"></div>
<script>
const $=id=>document.getElementById(id);
function toast(t){const e=$('toast');e.textContent=t;e.classList.add('on');clearTimeout(e.t);e.t=setTimeout(()=>e.classList.remove('on'),2600)}
async function api(u,o){const r=await fetch(u,o);const j=await r.json().catch(()=>({}));if(!r.ok&&j.erreur)throw new Error(j.erreur);return j}
function post(u,b){return api(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b||{})})}
async function cmd(c){if(!c.trim())return;try{const r=await post('/api/commande',{c});toast(r.reponse||'OK')}catch(e){toast(e.message)}}
let R={};
function remplir(r){R=r;for(const k in r){const e=$(k);if(!e||k==='code')continue;if(e.type==='checkbox')e.checked=!!r[k];else e.value=r[k]}
 $('titre').textContent=r.nom||'Komi';$('code').placeholder=r.code_regle?'déjà réglé (laisser vide pour garder)':'colle ici le code de relay.php';
 $('reseaux').innerHTML='';(r.reseaux||[]).forEach(s=>{const d=document.createElement('div');d.className='res';const n=document.createElement('span');n.textContent=s;const b=document.createElement('button');b.className='danger';b.textContent='Oublier';b.onclick=()=>post('/api/wifi',{ssid:s,oublier:true}).then(remplir);d.append(n,b);$('reseaux').append(d)});
 ['debit','volume','micro','luminosite'].forEach(k=>{$('v'+k).textContent=$(k).value})}
['debit','volume','micro','luminosite'].forEach(k=>$(k).addEventListener('input',()=>$('v'+k).textContent=$(k).value));
async function sauver(cles){const b={};cles.forEach(k=>{const e=$(k);if(e.type==='checkbox')b[k]=e.checked;else if(e.type==='range'||e.type==='number')b[k]=Number(e.value);else b[k]=e.value});
 if(cles.includes('code')&&!b.code)delete b.code;try{remplir(await post('/api/reglages',b));$('code').value='';toast('Enregistré ✓')}catch(e){toast(e.message)}}
function genCode(){const a='ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789';let s='';const r=crypto.getRandomValues(new Uint8Array(24));r.forEach(x=>s+=a[x%a.length]);$('code').value=s;
 toast('Copie ce code dans relay.php ($CODE_APPAREIL) puis Enregistre')}
async function tester(){$('tests').innerHTML='<div>Test en cours…</div>';try{const r=await api('/api/test');
 if(r.erreur){$('tests').innerHTML='';const d=document.createElement('div');d.className='ko';d.textContent=r.erreur;$('tests').append(d);return}
 $('tests').innerHTML='';(r.tests||[]).forEach(t=>{const d=document.createElement('div');const s=document.createElement('span');s.className=t.ok?'ok':'ko';s.textContent=(t.ok?'✓ ':'✗ ')+t.nom+' : '+t.detail;d.append(s);if(t.aide){const p=document.createElement('div');p.className='note';p.textContent=t.aide;d.append(p)}$('tests').append(d)})}
 catch(e){$('tests').textContent=e.message}}
async function scan(){toast('Recherche…');const l=await api('/api/scan');const s=$('ssid');s.innerHTML='<option value="">— Choisir —</option>';l.sort((a,b)=>b.force-a.force).forEach(r=>{const o=document.createElement('option');o.value=r.ssid;o.textContent=r.ssid+(r.ouvert?' (ouvert)':'');s.append(o)})}
async function ajouterWifi(){const ssid=$('ssidm').value||$('ssid').value;if(!ssid)return toast('Choisis un réseau');try{remplir(await post('/api/wifi',{ssid,mdp:$('mdp').value}));$('mdp').value='';toast('Ajouté : il va essayer de s\'y connecter')}catch(e){toast(e.message)}}
function maj(){const f=$('fichier').files[0];if(!f)return toast('Choisis le fichier komi-maj.bin');const x=new XMLHttpRequest();const fd=new FormData();fd.append('f',f);
 $('prog').hidden=false;x.upload.onprogress=e=>$('prog').value=e.loaded/e.total*100;x.onload=()=>toast(x.status==200?'Fini ! Il redémarre…':'Échec de la mise à jour');x.open('POST','/maj');x.send(fd)}
async function etat(){try{const e=await api('/api/etat');const c=[['Wi-Fi',e.wifi||'—'],['Adresse',e.ip||'—'],['Batterie',e.batterie],['Cerveau',e.cerveau],['Version',e.version],['Bluetooth',e.bluetooth]];
 $('etat').innerHTML='';c.forEach(([k,v])=>{const d=document.createElement('div');const b=document.createElement('b');b.textContent=k;d.append(b,document.createTextNode(v));$('etat').append(d)});
 $('sous').textContent=e.config?'Mode installation : ajoute ton Wi-Fi ci-dessous':'Réglages du boîtier'}catch(e){}}
[['Cyan','#35e0ff'],['Vert','#4ade80'],['Violet','#a78bfa'],['Rose','#ff6fb5'],['Orange','#ffa94d'],['Blanc','#f1f5f9'],['Rouge','#ff5b5b'],['Jaune','#ffe14d']].forEach(([n,c])=>{const b=document.createElement('button');b.textContent=n;b.style.borderLeft='14px solid '+c;b.onclick=()=>{$('couleur').value=c;sauver(['couleur','forme'])};$('palette').append(b)});
$('forme').addEventListener('change',()=>sauver(['forme']));
api('/api/reglages').then(remplir);etat();setInterval(etat,5000);
</script></body></html>)HTML";
