-- Troque 192.168.0.10 pelo IP do seu PC na rede Wi-Fi (ipconfig -> "Endereço IPv4").
USE acore_auth;
UPDATE realmlist SET address = '192.168.0.10', localAddress = '192.168.0.10' WHERE id = 1;
SELECT id, name, address, port, gamebuild FROM realmlist;
