/*
 * Packing of a machine state for the NVRAM. See stpack.h.
 */

#include "stpack.h"

#include "string.h"

#define LZ_MIN      3           /* shortest match in the state */
#define LZ_RMIN     6           /* shortest match into the image */
#define LZ_HBITS    13          /* hash of the state's 3-byte strings */
#define LZ_WIN      32768u      /* farthest state offset */
#define LZ_DEPTH    8           /* state positions tried per hash */
#define LZ_RDEPTH   8           /* image positions tried per probe */
#define LZ_MAXLEN   4096
#define LIT_BITS    3           /* context bits of a literal */
#define NIL         0xFFFFu
#define RING        32          /* image matches found ahead, power of two */
#define RH_BITS_MAX 14

/*----------------------------------------------------------------- coder */

/* Carry-less binary arithmetic coder over 32 bits: the interval [x1, x2]
 * is cut at a 12-bit probability, and the top byte goes out once both
 * ends share it. The encoder ends with the top byte of x1; the decoder
 * reads 255 past the end of its input. */
typedef struct {
    uint32 x1;
    uint32 x2;
    uint32 x;
    uint8 *p;
    uint8 *end;
} rc_t;

/* Probabilities of a 1 in 16 bits, kept in words (no halfword loads on
 * the ARM60), adapting by a sixteenth of the error. */
typedef struct {
    uint32 ismatch[2][STP_REGIONS][4];  /* [last token a match][region][position & 3] */
    uint32 lit[STP_REGIONS][1 << LIT_BITS][256];
    uint32 kind[2];                     /* rep or other; state or image */
    uint32 rcont;                       /* image continued */
    uint32 len[3][2][24];               /* Elias gamma: [rep, state, image][length, bits] */
    uint32 slot[2][32];                 /* offset bit length tree: state, image */
    uint32 obits[2][32][4];             /* the two bits under the top one */
} lz_model;

typedef struct {
    rc_t     rc;
    uint32   enc;
    lz_model m;
} lz_io;

/* Match finder: hash chains over the state (positions in 16 bits, the
 * chain links by position modulo the window), and an index of the image
 * every stride bytes, probed on demand up to a stride ahead of the
 * position asked: the matches found, extended both ways, wait in a ring
 * until passed. */
typedef struct {
    const stp_in *in;
    uint16 *head;
    uint16 *prev;
    uint16 *rhead;
    uint16 *rnext;
    uint32  rbits;
    uint32  stride;             /* 0: no image matches */
    uint32  nidx;
    uint32  at;                 /* next position probed */
    uint32  end;                /* end of the last image match found */
    uint32  rh;
    uint32  rt;
    uint32  r0[RING];           /* image matches: start, image address, end */
    uint32  ra[RING];
    uint32  re[RING];
} lz_mf;

void stp_lend(stp_mem *m, void *p, uint32 n)
{
    if (m->n < STP_PIECES && n >= 4) {
        m->piece[m->n].p = (uint8 *)p;
        m->piece[m->n].left = n & ~3u;
        m->n++;
    }
}

void *stp_take(stp_mem *m, uint32 n)
{
    uint32 best = m->n;
    uint32 k;
    uint8 *p;

    n = (n + 3) & ~3u;
    for (k = 0; k < m->n; k++) {
        if (m->piece[k].left >= n && (best == m->n || m->piece[k].left < m->piece[best].left))
            best = k;
    }
    if (best == m->n)
        return NULL;
    p = m->piece[best].p;
    m->piece[best].p += n;
    m->piece[best].left -= n;
    return p;
}

static void rc_enc(rc_t *c, uint32 bit, uint32 p)
{
    uint32 xmid = c->x1 + ((c->x2 - c->x1) >> 12) * p;

    if (bit)
        c->x2 = xmid;
    else
        c->x1 = xmid + 1;
    while (((c->x1 ^ c->x2) & 0xFF000000u) == 0) {
        if (c->p < c->end)
            *c->p = (uint8)(c->x2 >> 24);
        c->p++;
        c->x1 <<= 8;
        c->x2 = (c->x2 << 8) | 255;
    }
}

static uint32 rc_dec(rc_t *c, uint32 p)
{
    uint32 xmid = c->x1 + ((c->x2 - c->x1) >> 12) * p;
    uint32 bit = c->x <= xmid;

    if (bit)
        c->x2 = xmid;
    else
        c->x1 = xmid + 1;
    while (((c->x1 ^ c->x2) & 0xFF000000u) == 0) {
        c->x1 <<= 8;
        c->x2 = (c->x2 << 8) | 255;
        c->x = (c->x << 8) | (c->p < c->end ? *c->p++ : 255);
    }
    return bit;
}

static void model_init(lz_model *m)
{
    uint32 *p = (uint32 *)m;
    uint32 k;

    for (k = 0; k < sizeof(*m) / sizeof(uint32); k++)
        p[k] = 32768;
}

static uint32 bit_io(lz_io *z, uint32 *p, uint32 bit)
{
    uint32 q = (*p >> 4) | 1;

    if (z->enc)
        rc_enc(&z->rc, bit, q);
    else
        bit = rc_dec(&z->rc, q);
    if (bit)
        *p += (65536 - *p) >> 4;
    else
        *p -= *p >> 4;
    return bit;
}

static uint32 raw_io(lz_io *z, uint32 bit)
{
    if (z->enc)
        rc_enc(&z->rc, bit, 2048);
    else
        bit = rc_dec(&z->rc, 2048);
    return bit;
}

/* Elias gamma of v >= 1: the bit count in unary, then the bits under the
 * top one, all adaptive. */
static uint32 gamma_io(lz_io *z, uint32 *u, uint32 *b, uint32 v)
{
    uint32 nb = 0;
    uint32 k;
    uint32 r;

    if (z->enc) {
        while ((v >> (nb + 1)) != 0)
            nb++;
        for (k = 0; k < nb; k++)
            bit_io(z, &u[k < 23 ? k : 23], 1);
        bit_io(z, &u[nb < 23 ? nb : 23], 0);
        for (k = nb; k-- > 0;)
            bit_io(z, &b[k < 23 ? k : 23], (v >> k) & 1);
        return v;
    }
    while (nb < 31 && bit_io(z, &u[nb < 23 ? nb : 23], 0))
        nb++;
    r = 1;
    for (k = nb; k-- > 0;)
        r = (r << 1) | bit_io(z, &b[k < 23 ? k : 23], 0);
    return r;
}

/* An offset or an address: its bit length (a 5-bit tree), the two bits
 * under the top one adaptive, the others raw. */
static uint32 off_io(lz_io *z, uint32 t, uint32 v)
{
    uint32 nb = 0;
    uint32 node = 1;
    uint32 r = 1;
    uint32 k;
    uint32 bit;

    if (z->enc) {
        while ((v >> nb) != 0)
            nb++;
    }
    for (k = 5; k-- > 0;)
        node = (node << 1) | bit_io(z, &z->m.slot[t][node], (nb >> k) & 1);
    nb = node & 31;
    if (nb <= 1)
        return nb;
    for (k = nb - 1; k-- > 0;) {
        uint32 d = nb - 2 - k;

        bit = (v >> k) & 1;
        if (d < 2)
            bit = bit_io(z, &z->m.obits[t][nb][d == 0 ? 0 : 1 + (r & 1)], bit);
        else
            bit = raw_io(z, bit);
        r = (r << 1) | bit;
    }
    return r;
}

static uint32 region_of(const stp_in *in, uint32 i)
{
    uint32 r = 0;

    while (r < STP_REGIONS - 1 && i >= in->rb[r + 1])
        r++;
    return r;
}

/* The literal at i of the state s (read when packing, written when
 * unpacking): its context is the high bits of the byte of the tile row
 * above in the video RAM (region 2), of the byte before elsewhere. */
static void lit_io(lz_io *z, uint8 *s, uint32 i, uint32 rg)
{
    uint32 ctx = 0;
    uint32 *t;
    uint32 node = 1;
    uint32 c = z->enc ? s[i] : 0;
    uint32 k;

    if (rg == 2) {
        if (i >= 4)
            ctx = s[i - 4] >> (8 - LIT_BITS);
    } else if (i >= 1) {
        ctx = s[i - 1] >> (8 - LIT_BITS);
    }
    t = z->m.lit[rg][ctx];
    for (k = 8; k-- > 0;)
        node = (node << 1) | bit_io(z, &t[node], (c >> k) & 1);
    if (!z->enc)
        s[i] = (uint8)node;
}

/*--------------------------------------------------------- match finder */

static uint32 h3(const uint8 *p)
{
    return (((uint32)p[0] << 16 | (uint32)p[1] << 8 | p[2]) * 2654435761u) >> (32 - LZ_HBITS);
}

static uint32 h6(const uint8 *p, uint32 bits)
{
    uint32 a = (uint32)p[0] | (uint32)p[1] << 8 | (uint32)p[2] << 16 | (uint32)p[3] << 24;
    uint32 b = (uint32)p[4] | (uint32)p[5] << 8;

    return (((a * 2654435761u) ^ (b * 0x9E3779B1u + (a >> 15))) * 2246822519u) >> (32 - bits);
}

static void dict_index(lz_mf *f)
{
    const uint8 *d = f->in->dict;
    uint32 q = 0;
    uint32 k;

    for (k = 0; k < (1u << f->rbits); k++)
        f->rhead[k] = NIL;
    for (k = 0; k < f->nidx; k++, q += f->stride) {
        uint32 h = h6(d + q, f->rbits);

        f->rnext[k] = f->rhead[h];
        f->rhead[h] = (uint16)k;
    }
}

/* Probes the image index from the next position not probed up to x plus
 * a stride: a hit is extended back to the end of the last match found
 * and forward as far as it goes, and the probe goes on from its end. */
static void scan_to(lz_mf *f, uint32 x)
{
    const stp_in *in = f->in;
    const uint8 *s = in->s;
    const uint8 *d = in->dict;
    uint32 n = in->n;
    uint32 lim = x + f->stride;

    while (f->at + 8 <= n && f->at <= lim && f->rt - f->rh < RING) {
        uint32 i = f->at;
        uint32 c = f->rhead[h6(s + i, f->rbits)];
        uint32 depth = LZ_RDEPTH;
        uint32 bestl = 0;
        uint32 bj = 0;
        uint32 ba = 0;

        for (; c != NIL && depth-- > 0; c = f->rnext[c]) {
            uint32 q = c * f->stride;
            uint32 j = i;
            uint32 a = q;
            uint32 l;

            if (s[i] != d[q] || s[i + 1] != d[q + 1] || s[i + 2] != d[q + 2] ||
                s[i + 3] != d[q + 3] || s[i + 4] != d[q + 4] || s[i + 5] != d[q + 5])
                continue;
            while (j > f->end && a > 0 && s[j - 1] == d[a - 1]) {
                j--;
                a--;
            }
            for (l = i - j; j + l < n && a + l < in->dict_n && s[j + l] == d[a + l]; l++)
                ;
            if (l > bestl) {
                bestl = l;
                bj = j;
                ba = a;
            }
        }
        if (bestl >= LZ_RMIN) {
            uint32 e = f->rt & (RING - 1);

            f->r0[e] = bj;
            f->ra[e] = ba;
            f->re[e] = bj + bestl;
            f->rt++;
            f->end = bj + bestl;
            f->at = (f->end > i + 1) ? f->end : i + 1;
        } else {
            f->at++;
        }
    }
}

/* The image match covering x, if any: its bytes left from x and the
 * image address of x. Asked for x never decreasing. */
static uint32 scan_get(lz_mf *f, uint32 x, uint32 *addr)
{
    uint32 k;

    scan_to(f, x);
    while (f->rh != f->rt && f->re[f->rh & (RING - 1)] <= x)
        f->rh++;
    for (k = f->rh; k != f->rt; k++) {
        uint32 e = k & (RING - 1);

        if (f->r0[e] <= x && x < f->re[e]) {
            *addr = f->ra[e] + (x - f->r0[e]);
            return f->re[e] - x;
        }
    }
    return 0;
}

/* The longest match at i: kind 0 the last state offset, 1 a state
 * offset, 2 an image address, 3 the image continued; a longer match must
 * beat a cheaper one by its cost. */
static uint32 find(lz_mf *f, uint32 i, uint32 *kind, uint32 *val, uint32 rep, uint32 rnext_addr)
{
    const stp_in *in = f->in;
    const uint8 *s = in->s;
    uint32 n = in->n;
    uint32 best = 0;
    uint32 max = n - i;
    uint32 d;
    uint32 l;
    uint32 c;

    if (max > LZ_MAXLEN)
        max = LZ_MAXLEN;
    if (rep != 0 && rep <= i) {
        for (l = 0; l < max && s[i + l] == s[i - rep + l]; l++)
            ;
        if (l >= 2) {
            best = l;
            *kind = 0;
            *val = rep;
        }
    }
    if (rnext_addr != 0 && rnext_addr < in->dict_n) {
        uint32 lm = in->dict_n - rnext_addr;

        if (lm > max)
            lm = max;
        for (l = 0; l < lm && s[i + l] == in->dict[rnext_addr + l]; l++)
            ;
        if (l >= 4 && l > best + 1) {
            best = l;
            *kind = 3;
            *val = rnext_addr;
        }
    }
    if (i + LZ_MIN <= n) {
        uint32 depth = LZ_DEPTH;

        for (c = f->head[h3(s + i)]; c != NIL && depth-- > 0; c = f->prev[c & (LZ_WIN - 1)]) {
            d = i - c;
            if (d > LZ_WIN)
                break;
            for (l = 0; l < max && s[i + l] == s[c + l]; l++)
                ;
            if (l >= LZ_MIN && l > best + (d > 4096 ? 1 : 0) + (*kind == 0 && best ? 1 : 0)) {
                best = l;
                *kind = 1;
                *val = d;
            }
        }
    }
    if (f->stride != 0) {
        uint32 a = 0;

        l = scan_get(f, i, &a);
        if (l > max)
            l = max;
        if (l >= LZ_RMIN && l > best + 2) {
            best = l;
            *kind = 2;
            *val = a;
        }
    }
    return best;
}

static void mf_insert(lz_mf *f, uint32 i)
{
    if (i + LZ_MIN <= f->in->n) {
        uint32 h = h3(f->in->s + i);

        f->prev[i & (LZ_WIN - 1)] = f->head[h];
        f->head[h] = (uint16)i;
    }
}

/*------------------------------------------------------- pack and unpack */

int32 stp_pack(const stp_in *in, uint8 *out, uint32 cap, stp_mem *m, uint32 *stride)
{
    static const uint8 strides[4] = { 8, 16, 32, 64 };
    lz_io *z = (lz_io *)stp_take(m, sizeof(lz_io));
    lz_mf *f = (lz_mf *)stp_take(m, sizeof(lz_mf));
    uint16 *prev = (uint16 *)stp_take(m, 2 * LZ_WIN);
    uint16 *head = (uint16 *)stp_take(m, 2u << LZ_HBITS);
    uint8 *s = (uint8 *)in->s;
    uint32 i = 0;
    uint32 k;
    uint32 rep = 0;
    uint32 last = 0;
    uint32 rnext_addr = 0;
    uint32 len;

    *stride = 0;
    if (z == NULL || f == NULL || prev == NULL || head == NULL || in->n >= NIL)
        return STP_NOMEM;
    memset(f, 0, sizeof(*f));
    f->in = in;
    f->prev = prev;
    f->head = head;
    for (k = 0; k < (1u << LZ_HBITS); k++)
        head[k] = NIL;
    /* The image index, with the smallest stride the memory left holds. */
    if (in->dict != NULL && in->dict_n >= LZ_RMIN) {
        for (k = 0; k < 4 && f->stride == 0; k++) {
            stp_mem keep = *m;
            uint32 st = strides[k];
            uint32 nidx = (in->dict_n - LZ_RMIN) / st + 1;
            uint32 bits = 10;

            /* Positions indexed in 16 bits: a 512 KiB image loses its
             * last one at a stride of 8. */
            if (nidx > NIL + 1)
                continue;
            if (nidx > NIL)
                nidx = NIL;
            while (bits < RH_BITS_MAX && (1u << bits) < nidx / 2)
                bits++;
            f->rnext = (uint16 *)stp_take(m, 2 * nidx);
            f->rhead = (uint16 *)stp_take(m, 2u << bits);
            if (f->rnext != NULL && f->rhead != NULL) {
                f->stride = st;
                f->nidx = nidx;
                f->rbits = bits;
            } else {
                *m = keep;
            }
        }
        if (f->stride != 0)
            dict_index(f);
    }

    z->enc = 1;
    model_init(&z->m);
    z->rc.x1 = 0;
    z->rc.x2 = 0xFFFFFFFFu;
    z->rc.x = 0;
    z->rc.p = out;
    z->rc.end = out + cap;
    while (i < in->n) {
        uint32 kind = 0;
        uint32 val = 0;
        uint32 rg = region_of(in, i);
        uint32 l = find(f, i, &kind, &val, rep, rnext_addr);

        /* Lazy: a literal when a longer match starts one byte on. */
        if (l >= 2 && l < 64 && i + 1 < in->n) {
            uint32 k2 = 0;
            uint32 v2 = 0;

            if (find(f, i + 1, &k2, &v2, rep, rnext_addr) > l + 1)
                l = 0;
        }
        if (l < 2 || (kind == 1 && l < LZ_MIN)) {
            bit_io(z, &z->m.ismatch[last][rg][i & 3], 0);
            lit_io(z, s, i, rg);
            mf_insert(f, i);
            i++;
            last = 0;
            continue;
        }
        bit_io(z, &z->m.ismatch[last][rg][i & 3], 1);
        if (kind == 0) {
            bit_io(z, &z->m.kind[0], 0);
        } else {
            bit_io(z, &z->m.kind[0], 1);
            if (kind == 1) {
                bit_io(z, &z->m.kind[1], 0);
            } else {
                bit_io(z, &z->m.kind[1], 1);
                bit_io(z, &z->m.rcont, kind == 3);
            }
        }
        {
            uint32 t = (kind == 0) ? 0 : (kind == 1) ? 1 : 2;

            gamma_io(z, z->m.len[t][0], z->m.len[t][1], l - 1);
            if (kind == 1) {
                off_io(z, 0, val);
                rep = val;
            } else if (kind == 2) {
                off_io(z, 1, val);
            }
            if (kind >= 2)
                rnext_addr = val + l;
        }
        for (k = 0; k < l; k++)
            mf_insert(f, i + k);
        i += l;
        last = 1;
    }
    /* The top byte of x1 ends the stream (x1 and x2 differ there). */
    if (z->rc.p < z->rc.end)
        *z->rc.p = (uint8)(z->rc.x1 >> 24);
    z->rc.p++;
    len = (uint32)(z->rc.p - out);
    if (len > cap)
        return STP_LARGE;
    *stride = f->stride;
    return (int32)len;
}

int32 stp_unpack(const stp_in *in, uint8 *s, const uint8 *src, uint32 len, stp_mem *m)
{
    lz_io *z = (lz_io *)stp_take(m, sizeof(lz_io));
    uint32 n = in->n;
    uint32 i = 0;
    uint32 k;
    uint32 rep = 0;
    uint32 last = 0;
    uint32 rnext_addr = 0;

    if (z == NULL)
        return STP_NOMEM;
    z->enc = 0;
    model_init(&z->m);
    z->rc.x1 = 0;
    z->rc.x2 = 0xFFFFFFFFu;
    z->rc.x = 0;
    z->rc.p = (uint8 *)src;
    z->rc.end = (uint8 *)src + len;
    for (k = 0; k < 4; k++)
        z->rc.x = (z->rc.x << 8) | (z->rc.p < z->rc.end ? *z->rc.p++ : 255);
    while (i < n) {
        uint32 rg = region_of(in, i);
        uint32 kind;
        uint32 l;
        uint32 val = 0;

        if (!bit_io(z, &z->m.ismatch[last][rg][i & 3], 0)) {
            lit_io(z, s, i, rg);
            i++;
            last = 0;
            continue;
        }
        if (!bit_io(z, &z->m.kind[0], 0))
            kind = 0;
        else if (!bit_io(z, &z->m.kind[1], 0))
            kind = 1;
        else
            kind = bit_io(z, &z->m.rcont, 0) ? 3 : 2;
        {
            uint32 t = (kind == 0) ? 0 : (kind == 1) ? 1 : 2;

            l = gamma_io(z, z->m.len[t][0], z->m.len[t][1], 0) + 1;
        }
        if (l > n - i)
            return STP_BAD;
        if (kind == 0) {
            val = rep;
        } else if (kind == 1) {
            val = off_io(z, 0, 0);
            rep = val;
        } else if (kind == 2) {
            val = off_io(z, 1, 0);
        } else {
            val = rnext_addr;
        }
        if (kind <= 1) {
            if (val == 0 || val > i)
                return STP_BAD;
            for (k = 0; k < l; k++)
                s[i + k] = s[i + k - val];
        } else {
            if (in->dict == NULL || val > in->dict_n || l > in->dict_n - val)
                return STP_BAD;
            for (k = 0; k < l; k++)
                s[i + k] = in->dict[val + k];
            rnext_addr = val + l;
        }
        i += l;
        last = 1;
    }
    return 0;
}
