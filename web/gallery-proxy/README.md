# SwitchU gallery proxy

Servidor read-only para a galeria SteamGridDB do SwitchU. Ele mantém a chave da
API somente no LXC e expõe os dados mínimos ao menu.

## Produção

- LXC Proxmox: `<container-id>` (`switchu-gallery`), `<proxy-host-ip>`, porta HTTP `8081`.
- Endereço público: `https://gallery.nclabs.dev` pelo Cloudflare Tunnel.
- Código no LXC: `/opt/switchu-gallery`.
- Segredo: `/etc/switchu-gallery/steamgriddb.env`, modo `0600`, nunca versionar.

O Public Hostname do Tunnel deve apontar para `http://<proxy-host-ip>:8081`.

## Endpoints públicos

- `GET /health`
- `GET /v1/search?query=<nome do jogo>`
- `GET /v1/games/<id>/grids?dimensions=600x900`
- `GET /v1/games/<id>/heroes?dimensions=1920x620`

As dimensões aceitas são as do filtro do SteamGridDB: Grids `600x900`,
`342x482`, `660x930`, `512x512`, `1024x1024`, `460x215`, `920x430`; Fundos
`1920x620`, `3840x1240`, `1600x650`. O proxy aceita apenas um valor por vez,
mantendo cache e custo da API previsíveis.

## Chave SteamGridDB

Crie uma chave API na conta SteamGridDB e grave no LXC:

```sh
install -d -m 0750 /etc/switchu-gallery
printf 'STEAMGRIDDB_API_KEY=cole_a_chave_aqui\n' > /etc/switchu-gallery/steamgriddb.env
chmod 0600 /etc/switchu-gallery/steamgriddb.env
systemctl restart switchu-gallery
```

A chave nunca vai para o SwitchU, para o Git, nem para logs. Sem ela, `/health`
continua funcional e os endpoints de catálogo retornam `503`.
