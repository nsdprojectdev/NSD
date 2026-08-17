# BUGS_FIXED

NSD geliştirme sürecinde tespit edilip düzeltilen hataların kaydı.

---

## 2026-08-17 — sysfs debug arayüzünde buffer overflow (sprintf → scnprintf)

**Dosya:** `nsd.c` (9 çağrı noktası)

**Belirti:** Uzun süreli kullanımda (4.5 gün uptime, NSD yüklü) `drop_caches`
sırasında kernel oops + RCU stall + tam sistem donması:

```
Oops: general protection fault, probably for non-canonical address 0x6f635f64616576d0
RIP: lru_gen_clear_refs+0x8f/0x100  (MGLRU)
RIP: memcg_list_lru_alloc+0xd5/0x220 (socket inode alloc)
```

Oops adreslerinin ASCII çözümlemesi (`read_count`, `=20874`, `ot=1049`)
taşan verinin `fctx_debug` çıktısının parçası olduğunu gösterdi.

**Kök neden:** `fctx_debug_show()` fonksiyonu 256 fctx slotunun her biri için
~110 karakterlik satır üretiyordu (`sprintf(buf + len, ...)` — sınırsız yazma).
Kernel sysfs read buffer yalnızca PAGE_SIZE (4096 byte). 256 × 110 ≈ 29 KB
yazım girişimi buffer taşmasına ve çekirdek bellek bozulmasına yol açtı.
Diğer 8 sysfs show fonksiyonunda da aynı desen (sınırsız `sprintf`, sabit
buffer) mevcuttu; daha küçük çıktı ürettikleri için henüz tetiklenmemişti.

**Tespit ediliş şekli:** Dump'lanan oops register'larındaki ASCII string'lerin
(`read_co` = `read_count` alan adı) `fctx_debug` formatıyla eşleşmesi +
`kobj_attr_show returned bad count` uyarısının oops'tan 56 sn önce gelmesi.

**Düzeltme:** Dosyadaki tüm `sprintf(buf, ...)` çağrıları güvenli
`scnprintf(buf, PAGE_SIZE, ...)` ile değiştirildi; `fctx_debug_show()` ayrıca
`len >= PAGE_SIZE` sınırında döngüyü kırar. Sonuç: çıktı tam 4095 byte'ta
kesiliyor, taşma imkânsız.

**Doğrulama:** Düzeltme sonrası 100× `cat /sys/kernel/nsd/fctx_debug` +
50× tüm diğer sysfs dosyaları stres okuması: 0 oops, 0 BUG, 0 uyarı.
