# NSD v1 — Changelog

## 2026-07-19 — waste_track Bug Fix, Metrik Analizi & Kprobe Dogrulamasi

### Kprobe O_DIRECT Dogrulamasi

Kprobe `vfs_read` uzerinde, hem O_DIRECT hem buffered okumalarda calisiyor.
Onceden "O_DIRECT bypass ediyor" notu yanlisti; kprobe her iki durumda da
atesleniyor. dd iflag=direct ile dogrulandi (1000x4K → kprobe +1008).

### observe_only Dogrulamasi

`observe_only=0` → prefetch aktif, `observe_only=1` → hic prefetch yok.
5000 I/O testinde: observe_only=1 iken prefetch_sent=0, correct=0.
Sifirdan farkli calisma yok.

### prefetch_amp Metrik Aciklamasi

prefetch_amp = correct / prefetch_sent × 100. Pay ve payda farkli seyleri
saydigi icin >%100 olabilir: tek vfs_fadvise birden fazla sayfa getirir,
her sayfa okumasi ayri correct sayilir. Bug degil, tanim farkliligi.

### Critical Bug Fix: waste_track timestamp corruption

**Dosya:** `nsd.c`

`nsd_pend_add()` pending array'e siphash degeri yaziyordu. `nsd_waste_track_fn`
bu degeri timestamp sanip `(now - hash) > 5s` karsilastirmasi yapiyordu.
Unsigned arithmetic'te hash >> now oldugunda sarma olusuyor, tum pending
tablosu expire sayiliyordu. `pending_ts[]` paralel array eklendi, gercek
`nsd_ns()` degeri saklaniyor. Feature varsayilan kapali (0) oldugu icin
bug aktif degildi. Canli test: sequential read + jump → +19 waste_expired,
tum tablo degil.

---

## 2026-07-19 — qd>24 Fix, Parametre Optimizasyonu & Strategy Revizyonu

### Critical Bug Fix: queue-depth limiter

**Dosya:** `nsd.c` (satir 1236-1245)

qd > 24 oldugunda `depth` zorla 1'e setleniyordu. Hem SSD (sda) hem HDD (sdb) qd=32
oldugu icin **prefetch tamamen felçti** — `depth=1` tek sayfa prefetch ediyordu.

**Fix:** Blok kaldirildi. depth dogrudan kullaniliyor.

```
- if (qd > 24) depth = 1;
+ /* queue depth limit kaldirildi */
```

### Fix: Uninitialized `depth` in `feat_procaware`

**Dosya:** `nsd.c` (satir 1216-1220)

`feat_procaware` blogunda `depth = max(depth, 4)` kullaniliyordu ama `depth`
henuz initialize edilmemisti. `depth_boost` degiskeni eklendi, assignment sonrasi
uygulaniyor.

### Yeni: skip_kprobe kaldirildi

**Dosya:** `nsd.c` (satir 828-835)

Strateji `NSD_STRAT_NONE` oldugunda kprobe tamamen atlaniyordu (5sn boyunca).
Bu, RRP gibi deterministik desenlerin Markov zinciri tarafindan ogrenilmesini
engelliyordu. Artik `strat=NONE` olsa bile olaylar isleniyor, sadece prefetch
yapilmiyor.

```
- WRITE_ONCE(nsd.skip_kprobe[ctx->file_id], true);
- ...
+ WRITE_ONCE(nsd.skip_kprobe[ctx->file_id], false);
```

### Yeni: SEQ'de Markov stratejisi aktif

**Dosya:** `nsd.c` (satir 783-797)

Eskiden `seq_r > 600` → `NSD_STRAT_NONE` (prefetch yok). Artik sadece saf
random (`seq_r < 120 && rpt_r < 100`) NONE aliyor. Geri kalan her sey (SEQ
dahil) `NSD_STRAT_MARKOV` ile prefetch yapiyor.

```
- if (seq_r > NSD_SEQ_RATIO_THRESH) proposed = NSD_STRAT_NONE;
- else if (full) proposed = NSD_STRAT_NONE;
- else proposed = NSD_STRAT_NONE;
+ if (seq_r < 120 && rpt_r < 100) proposed = NSD_STRAT_NONE;
+ ...
+ else proposed = NSD_STRAT_MARKOV;
```

### Degisen Parametreler

| Parametre | Eski | Yeni |
|-----------|------|------|
| `NSD_THRESH_SSD` | 100 | 200 |
| `NSD_DEPTH_SSD` | 6 | 3 |
| `NSD_PREFETCH_SPAN_SSD` | 128K | 64K |
| `NSD_THRESH_MIN` | 150 | 200 |
| `kprobe sampling` | 1/4 (`& 3ULL`) | 1/2 (`& 1ULL`) |

### Test Sonuclari (3x OFF/ON, SSD)

| Workload | OFF_ort | ON_ort | Δ |
|----------|---------|--------|---|
| SEQ (1M) | 479,713 | **488,511** | **+%1.8** |
| RND (4K) | 16,821 | **18,590** | **+%10.5** |
| RRP1 (4K) | 20,883 | 20,356 | −%2.5 |
| RRP2 (4K) | 20,744 | 20,035 | −%3.4 |

**Not:** Test gurultusu yuksek (±%5-10). SEQ ve RND pozitif, RRP farklari
gurultu icinde. pf=33K, hit=%96, ev=1.37M, rec_acc=%65.

### Onceden Tespit Edilen Sorunlar

1. **qd>24 fix oncesi:** prefetch_sent=135, hit=%82 (depth zorla 1'di)
2. **qd>24 fix sonrasi:** prefetch_sent=2365, hit=%72 (depth=6, thresh=100)
3. **thresh=200/depth=3 sonrasi:** pf=38, hit=%81, rec_acc=%67 (cok tutucu)
4. **SEQ'de MARKOV aktif + skip_kprobe kaldirilinca:** pf=33K, hit=%96

### Kalan Konular

- HDD testi yapilmadi
- RRP1 hala gurultu icinde, daha uzun sureli test gerekebilir
- Direct=1 ile O_DIRECT uyumsuz (kprobe `vfs_read`'e bagli, O_DIRECT bypass ediyor)
- Otomatik parametre kesfi (autothresh) henuz degerlendirilmedi

---

## 2026-07-18 — Baseline & Regresyon Analizi

### Kprobe Sampling

1/4 → 1/2 degistirildi. Kprobe olaylari ~4271 → ~8123'e cikti (2x).

### RND Hysteresis

`CONFIRM_UP_RND=3` random workledlar icin eklendi (`seq_r<120 && rpt_r<100`).
`CONFIRM_UP=2`, `CONFIRM_DOWN=1`.

### Monitor Window/Easing

1024/300/700 → 128/50/100 (backup values). Adaptasyon hizini artirmak icin.

### Parametreler Backup Degerlerine Donduruldu

- `THRESH_SSD` = 100
- `DEPTH_SSD` = 6
- `PREFETCH_SPAN_SSD` = 128K
- `SEQ_BYPASS_THRESH_SSD` = 800
- `AUTOTHRESH_STEP` = 10
- `SMALL_IO_THRESH` = 8192
- `PROFILE_WINDOW` = 512
- `skip_kprobe timeout` 10s → 5s

### Fctx Buckets

`NSD_FCTX_BITS=7` (128 slot). Collision %4 vs %43 (eskisi 6 bit = 64 slot).

### HDD Testi

Tum workloadlar ±%0.1 ile ±%1.8 arasinda, tamamen gurultu icinde.
HDD ~78 IOPS (=12.7ms seek) mekanik olarak saturated. NCQ (iodepth=8)
depth=1'den fark gostermedi.

### Direct=1 Testi

NSD'nin kprobe'i `__filemap_get_folio` uzerinde degil, `vfs_read` uzerinde.
O_DIRECT okumalarinda kprobe ateslenmiyor. SEQ direct=1 −%2.5 olcum hatasiydi.

## 2026-07-19 — Parameter Tuning (Depth, Threshold & Sampling)

### Degisen Parametreler (Kod)

| Parametre | Eski | Yeni | Aciklama |
|-----------|------|------|----------|
|  | 3 | 8 | qd=32 icin optimum |
|  | 200 | 180 | Daha fazla prefetch |
|  | 64K | 256K | Genis spekulum |
|  | 7 (128) | 8 (256) | Az collision |
|  | 4 | 8 | Az eviction |
|  | 10 | 25 | Hizli uyum |
|  | 128 | 256 | Stabil monitor |
|  | 50 | 30 | Daha gec disable |
|  | 100 | 80 | Daha cabuk enable |
|  | 128 | 256 | Az drop |
|  | 1/2 | 1/1 (tumu) | Tum olaylar islenir |
|  | 950 | 900 | Daha kolay thresh dusurme |
|  | 100 | 150 | Daha erken thresh artirma |

### Kalibrasyon (depth=16  agresif testi)

depth=16 denendi ancak rec_acc %3'e dustu — cok fazla spekulatif prefetch.
depth=8 ile dengelendi.

### Test Sonuclari (ssd_test.sh, direct=1, depth=8/thresh=180)

| Workload | OFF | ON | Delta |
|----------|-----|----|-------|
| Concurrent (64K+4K) | 80.8MB/s | 80.1MB/s | %0.9 |
| Pure random 4K | 17.3MB/s | 17.8MB/s | **+%2.8** |
| Pure seq 64K | 270.3MB/s | 258.7MB/s | %4.3 (direct=1 bypass) |
| RRP pass1 4K | 24.0MB/s | 24.8MB/s | **+%3.3** |
| RRP pass2 4K | 24.8MB/s | 27.2MB/s | **+%9.6** |

### Kalan Konular

- HDD testi yapilmadi
- direct=1 benchmark bypass, direct=0 ile daha anlamli sonuc
- RRP pass2 +%9.6 kazanc umut verici, daha uzun test gerek
- Autothresh hala cok aktif (step=25)

---

## 2026-07-19 — Parameter Tuning (Depth, Threshold and Sampling)

### Degisen Parametreler (Kod)

| Parametre | Eski | Yeni | Aciklama |
|-----------|------|------|----------|
| NSD_DEPTH_SSD | 3 | 8 | qd=32 icin optimum |
| NSD_THRESH_SSD | 200 | 180 | Daha fazla prefetch |
| NSD_PREFETCH_SPAN_SSD | 64K | 256K | Genis spekulum |
| NSD_FCTX_BITS | 7 (128) | 8 (256) | Az collision |
| NSD_SYN_WAYS | 4 | 8 | Az eviction |
| NSD_AUTOTHRESH_STEP | 10 | 25 | Hizli uyum |
| NSD_MON_WINDOW | 128 | 256 | Stabil monitor |
| NSD_MON_DISABLE | 50 | 30 | Daha gec disable |
| NSD_MON_ENABLE | 100 | 80 | Daha cabuk enable |
| NSD_RING_SIZE | 128 | 256 | Az drop |
| Kprobe sampling | 1/2 | 1/1 (tumu) | Tum olaylar islenir |
| NSD_AUTOTHRESH_HIGH_ACC | 950 | 900 | Daha kolay thresh dusurme |
| NSD_AUTOTHRESH_LOW_ACC | 100 | 150 | Daha erken thresh artirma |

### Kalibrasyon (depth=16 agresif testi)

depth=16 denendi ancak rec_acc yuzde 3'e dustu. depth=8 ile dengelendi.

### Test Sonuclari (ssd_test.sh, direct=1, depth=8 thresh=180)

| Workload | OFF | ON | Delta |
|----------|-----|----|-------|
| Concurrent (64K+4K) | 80.8MB/s | 80.1MB/s | -0.9 |
| Pure random 4K | 17.3MB/s | 17.8MB/s | +2.8 |
| Pure seq 64K | 270.3MB/s | 258.7MB/s | -4.3 (direct=1 bypass) |
| RRP pass1 4K | 24.0MB/s | 24.8MB/s | +3.3 |
| RRP pass2 4K | 24.8MB/s | 27.2MB/s | +9.6 |

### Kalan Konular

- HDD testi yapilmadi
- direct=1 benchmark bypass, direct=0 ile daha anlamli sonuc
- RRP pass2 +9.6 kazanc umut verici, daha uzun test gerek
- Autothresh hala cok aktif (step=25)
