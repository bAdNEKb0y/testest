# Nexxus Compagnon — version Vercel

## Mettre en ligne (le plus simple, sans rien installer)
1. Sur github.com, crée un dépôt vide `nexxus-compagnon`, puis « uploading an existing file » : glisse TOUT le contenu de ce dossier (le dossier `api` compris).
2. Sur vercel.com → Add New… → Project → importe `nexxus-compagnon` → Deploy (laisse « Other » comme framework).

Ou, si Node est installé sur ton ordi : ouvre un terminal dans ce dossier et tape `npx vercel --prod`.

## Brancher le cerveau, la voix et les transports
Vercel → ton projet → Settings → Environment Variables, puis Deployments → ⋯ → Redeploy :

| Nom | Valeur |
|---|---|
| PHRASE_SECRETE | ta phrase pour le déverrouiller (obligatoire dès qu'une clé est mise) |
| ANTHROPIC_API_KEY | ta clé de console.anthropic.com |
| PRIM_KEY | ton jeton prim.iledefrance-mobilites.fr (transports) |
| AZURE_KEY | ta clé Azure « Speech » (voix réalistes, facultatif) |
| AZURE_REGION | facultatif, `francecentral` par défaut |
| MODELE | facultatif, `claude-haiku-5-5` par défaut (le moins cher) |

Ensuite, sur le site : déverrouille → ⋯ → Réglages → « Tester l'installation ».

Tes rappels, ta liste et ce qu'il retient sur toi restent sur ton appareil (rien n'est stocké sur le serveur).
