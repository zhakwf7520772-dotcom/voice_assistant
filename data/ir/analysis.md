# IR log analysis

Captures: 73; sessions: 3.
Observed valid 8-byte captures: 70. Count distribution: {70: 70, 25: 1, 2: 1, 1: 1}.
Inferred short/long spaces: 555/1636 us; decision threshold: 1095.5 us.

## Measured structure

Each recognized capture contains a header, a 35-bit timing block, an internal gap, a 32-bit timing block, and a terminal mark.
The 35-bit block separates into 32 payload bits and a 3-bit separator `010`. Each capture therefore has 64 payload bits (8 bytes), not 70 data bits.
Bytes below use LSB-first packing; printed bit strings retain transmission order. RMT zero-based symbol indices and one-based payload bit indices are different.
Two adjacent captures form one observed 16-byte operation. They carry different payloads.

## Field validation against annotations

{"annotated_operations_checked": 33, "errors": [], "same_state_payload_consistent": true, "unique_annotated_states": 9}

- Power candidate: byte 0 bit 3 (payload bit 4; RMT Symbol[4]).
- Temperature candidate: byte 1 low nibble + 16 (payload bits 9..12); only 25..27 C tested.
- Basic fan candidate: byte 0 bits 4..5 (payload bits 5..6), equal to min(fan, 3). Bit 4 of the transmission is POWER, not fan.
- Extended fan candidate: second capture byte 6 bits 4..6 (payload bits 53..55; RMT Symbols[57..59]; second internal block bits 21..23).
- Mode byte 0 bits 0..2 remain 1 in annotated samples. Cooling was held fixed, so other mode mappings are unverified.

## Checksum candidate

For each 8-byte capture: `C = (seed + sum(B[0..3] & 0xF) + sum(B[4..6] >> 4)) & 0xF`; compare with `B[7] >> 4`.
Seeds fitting all structurally valid captures: [10]. Checksum passes: 70/70.
The last byte's low nibble is always zero here. These samples cannot establish its role, timer behavior, other modes, or every temperature/fan combination.
This candidate was cross-checked with the primary IRremoteESP8266 Kelvinator implementation after extracting raw data: [checksum and framing](https://github.com/crankyoldgit/IRremoteESP8266/blob/master/src/ir_Kelvinator.cpp), [field layout](https://github.com/crankyoldgit/IRremoteESP8266/blob/master/src/ir_Kelvinator.h).
The observed two-capture / five-speed structure matches that protocol family; the remote model has not been established.

## Operations

| Session/op | Captures | Annotation | Gap us | First / second bytes |
|---|---|---|---|---|
| 1/1 | 1,2 | power=True temp=25 fan=1 | [40042] | 19 09 20 50 00 40 00 00 / 19 09 20 70 00 00 10 D0 |
| 1/2 | 3,4 | power=False temp=25 fan=1 | [40046] | 11 09 20 50 00 40 00 80 / 11 09 20 70 00 00 10 50 |
| 1/3 | 5,6 | power=True temp=25 fan=1 | [40044] | 19 09 20 50 00 40 00 00 / 19 09 20 70 00 00 10 D0 |
| 1/4 | 7,8 | power=False temp=25 fan=1 | [40017] | 11 09 20 50 00 40 00 80 / 11 09 20 70 00 00 10 50 |
| 1/5 | 9,10 | power=True temp=25 fan=1 | [40044] | 19 09 20 50 00 40 00 00 / 19 09 20 70 00 00 10 D0 |
| 1/6 | 11,12 | power=False temp=25 fan=1 | [39993] | 11 09 20 50 00 40 00 80 / 11 09 20 70 00 00 10 50 |
| 1/7 | 13,14,15 | unlabelled | [59440, 55098] | BD / undecodable / undecodable |
| 1/8 | 17,18 | unlabelled | [40013] | 11 09 20 50 00 40 00 80 / 11 09 20 70 00 00 10 50 |
| 2/1 | 1,2 | power=True temp=25 fan=1 | [39992] | 19 09 20 50 00 40 00 00 / 19 09 20 70 00 00 10 D0 |
| 2/2 | 3,4 | power=True temp=26 fan=1 | [40018] | 19 0A 20 50 00 40 00 10 / 19 0A 20 70 00 00 10 E0 |
| 2/3 | 5,6 | power=True temp=27 fan=1 | [40015] | 19 0B 20 50 00 40 00 20 / 19 0B 20 70 00 00 10 F0 |
| 2/4 | 7,8 | power=True temp=26 fan=1 | [40016] | 19 0A 20 50 00 40 00 10 / 19 0A 20 70 00 00 10 E0 |
| 2/5 | 9,10 | power=True temp=25 fan=1 | [40015] | 19 09 20 50 00 40 00 00 / 19 09 20 70 00 00 10 D0 |
| 2/6 | 11,12 | power=True temp=26 fan=1 | [40017] | 19 0A 20 50 00 40 00 10 / 19 0A 20 70 00 00 10 E0 |
| 2/7 | 13,14 | power=True temp=27 fan=1 | [39959] | 19 0B 20 50 00 40 00 20 / 19 0B 20 70 00 00 10 F0 |
| 2/8 | 15,16 | power=True temp=26 fan=1 | [39997] | 19 0A 20 50 00 40 00 10 / 19 0A 20 70 00 00 10 E0 |
| 2/9 | 17,18 | power=True temp=25 fan=1 | [40037] | 19 09 20 50 00 40 00 00 / 19 09 20 70 00 00 10 D0 |
| 2/10 | 19,20 | power=True temp=26 fan=1 | [40015] | 19 0A 20 50 00 40 00 10 / 19 0A 20 70 00 00 10 E0 |
| 2/11 | 21,22 | power=True temp=27 fan=1 | [39988] | 19 0B 20 50 00 40 00 20 / 19 0B 20 70 00 00 10 F0 |
| 2/12 | 23,24 | power=True temp=26 fan=1 | [40020] | 19 0A 20 50 00 40 00 10 / 19 0A 20 70 00 00 10 E0 |
| 2/13 | 25,26 | power=True temp=25 fan=1 | [40014] | 19 09 20 50 00 40 00 00 / 19 09 20 70 00 00 10 D0 |
| 2/14 | 27,28 | power=False temp=25 fan=1 | [40041] | 11 09 20 50 00 40 00 80 / 11 09 20 70 00 00 10 50 |
| 3/1 | 1,2 | power=True temp=25 fan=1 | [40039] | 19 09 20 50 00 40 00 00 / 19 09 20 70 00 00 10 D0 |
| 3/2 | 3,4 | power=True temp=25 fan=2 | [40018] | 29 09 20 50 00 40 00 00 / 29 09 20 70 00 00 20 E0 |
| 3/3 | 5,6 | power=True temp=25 fan=3 | [40041] | 39 09 20 50 00 40 00 00 / 39 09 20 70 00 00 30 F0 |
| 3/4 | 7,8 | power=True temp=25 fan=4 | [40038] | 39 09 20 50 00 40 00 00 / 39 09 20 70 00 00 40 00 |
| 3/5 | 9,10 | power=True temp=25 fan=5 | [40039] | 39 09 20 50 00 40 00 00 / 39 09 20 70 00 00 50 10 |
| 3/6 | 11,12 | power=True temp=25 fan=0 | [40038] | 09 09 20 50 00 40 00 00 / 09 09 20 70 00 00 00 C0 |
| 3/7 | 13,14 | power=True temp=25 fan=1 | [40039] | 19 09 20 50 00 40 00 00 / 19 09 20 70 00 00 10 D0 |
| 3/8 | 15,16 | power=True temp=25 fan=2 | [40014] | 29 09 20 50 00 40 00 00 / 29 09 20 70 00 00 20 E0 |
| 3/9 | 17,18 | power=True temp=25 fan=3 | [39990] | 39 09 20 50 00 40 00 00 / 39 09 20 70 00 00 30 F0 |
| 3/10 | 19,20 | power=True temp=25 fan=4 | [39991] | 39 09 20 50 00 40 00 00 / 39 09 20 70 00 00 40 00 |
| 3/11 | 21,22 | power=True temp=25 fan=5 | [39989] | 39 09 20 50 00 40 00 00 / 39 09 20 70 00 00 50 10 |
| 3/12 | 23,24 | power=True temp=25 fan=0 | [40012] | 09 09 20 50 00 40 00 00 / 09 09 20 70 00 00 00 C0 |
| 3/13 | 25,26 | power=True temp=25 fan=1 | [40011] | 19 09 20 50 00 40 00 00 / 19 09 20 70 00 00 10 D0 |
| 3/14 | 27,28 | unlabelled | [40040] | 11 09 20 50 00 40 00 80 / 11 09 20 70 00 00 10 50 |

## Observed fixed bits by capture phase

These masks describe this dataset, not every mode or remote function. A 1 in the mask means the bit did not change.

| Phase | Byte (zero-based) | Fixed mask | Fixed value |
|---|---|---|---|
| 1 | 0 | C7 | 01 |
| 1 | 1 | FC | 08 |
| 1 | 2 | FF | 20 |
| 1 | 3 | FF | 50 |
| 1 | 4 | FF | 00 |
| 1 | 5 | FF | 40 |
| 1 | 6 | FF | 00 |
| 1 | 7 | 4F | 00 |
| 2 | 0 | C7 | 01 |
| 2 | 1 | FC | 08 |
| 2 | 2 | FF | 20 |
| 2 | 3 | FF | 70 |
| 2 | 4 | FF | 00 |
| 2 | 5 | FF | 00 |
| 2 | 6 | 8F | 00 |
| 2 | 7 | 0F | 00 |

## Full capture data

### Session 1 capture 1

Symbols: 70; duration: 125832 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 50 00 40 00 00`.
Payload transmission bits: `1001100010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 1 capture 2

Symbols: 70; duration: 130158 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 70 00 00 10 D0`.
Payload transmission bits: `1001100010010000000001000000111000000000000000000000100000001011`.
Blocks (including separators): `10011000100100000000010000001110010 | 00000000000000000000100000001011`.
Issues: [].

### Session 1 capture 3

Symbols: 70; duration: 125837 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 50 00 40 00 80`.
Payload transmission bits: `1000100010010000000001000000101000000000000000100000000000000001`.
Blocks (including separators): `10001000100100000000010000001010010 | 00000000000000100000000000000001`.
Issues: [].

### Session 1 capture 4

Symbols: 70; duration: 127764 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 70 00 00 10 50`.
Payload transmission bits: `1000100010010000000001000000111000000000000000000000100000001010`.
Blocks (including separators): `10001000100100000000010000001110010 | 00000000000000000000100000001010`.
Issues: [].

### Session 1 capture 5

Symbols: 70; duration: 125828 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 50 00 40 00 00`.
Payload transmission bits: `1001100010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 1 capture 6

Symbols: 70; duration: 130142 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 70 00 00 10 D0`.
Payload transmission bits: `1001100010010000000001000000111000000000000000000000100000001011`.
Blocks (including separators): `10011000100100000000010000001110010 | 00000000000000000000100000001011`.
Issues: [].

### Session 1 capture 7

Symbols: 70; duration: 125835 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 50 00 40 00 80`.
Payload transmission bits: `1000100010010000000001000000101000000000000000100000000000000001`.
Blocks (including separators): `10001000100100000000010000001010010 | 00000000000000100000000000000001`.
Issues: [].

### Session 1 capture 8

Symbols: 70; duration: 127987 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 70 00 00 10 50`.
Payload transmission bits: `1000100010010000000001000000111000000000000000000000100000001010`.
Blocks (including separators): `10001000100100000000010000001110010 | 00000000000000000000100000001010`.
Issues: [].

### Session 1 capture 9

Symbols: 70; duration: 125837 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 50 00 40 00 00`.
Payload transmission bits: `1001100010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 1 capture 10

Symbols: 70; duration: 130140 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 70 00 00 10 D0`.
Payload transmission bits: `1001100010010000000001000000111000000000000000000000100000001011`.
Blocks (including separators): `10011000100100000000010000001110010 | 00000000000000000000100000001011`.
Issues: [].

### Session 1 capture 11

Symbols: 70; duration: 125855 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 50 00 40 00 80`.
Payload transmission bits: `1000100010010000000001000000101000000000000000100000000000000001`.
Blocks (including separators): `10001000100100000000010000001010010 | 00000000000000100000000000000001`.
Issues: [].

### Session 1 capture 12

Symbols: 70; duration: 127990 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 70 00 00 10 50`.
Payload transmission bits: `1000100010010000000001000000111000000000000000000000100000001010`.
Blocks (including separators): `10001000100100000000010000001110010 | 00000000000000000000100000001010`.
Issues: [].

### Session 1 capture 13

Symbols: 25; duration: 107593 us; structure: [4, 15, 2]; checksum: None.
Payload: `BD`.
Payload transmission bits: `10111101`.
Blocks (including separators): `0000 | 101111011111111 | 10`.
Issues: ['unclassified timing at symbol 0', 'unclassified timing at symbol 1', 'unclassified timing at symbol 11', 'unclassified timing at symbol 12', 'unclassified timing at symbol 13', 'unclassified timing at symbol 14', 'unclassified timing at symbol 15', 'unclassified timing at symbol 19', 'unclassified timing at symbol 21', 'unclassified timing at symbol 22', 'unclassified timing at symbol 5', 'unclassified timing at symbol 6'].

### Session 1 capture 14

Symbols: 2; duration: 1895 us; structure: [1]; checksum: None.
Payload: ``.
Payload transmission bits: ``.
Blocks (including separators): `1`.
Issues: [].

### Session 1 capture 15

Symbols: 1; duration: 278 us; structure: []; checksum: None.
Payload: ``.
Payload transmission bits: ``.
Blocks (including separators): ``.
Issues: [].

### Session 1 capture 17

Symbols: 70; duration: 125756 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 50 00 40 00 80`.
Payload transmission bits: `1000100010010000000001000000101000000000000000100000000000000001`.
Blocks (including separators): `10001000100100000000010000001010010 | 00000000000000100000000000000001`.
Issues: [].

### Session 1 capture 18

Symbols: 70; duration: 127960 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 70 00 00 10 50`.
Payload transmission bits: `1000100010010000000001000000111000000000000000000000100000001010`.
Blocks (including separators): `10001000100100000000010000001110010 | 00000000000000000000100000001010`.
Issues: [].

### Session 2 capture 1

Symbols: 70; duration: 125848 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 50 00 40 00 00`.
Payload transmission bits: `1001100010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 2 capture 2

Symbols: 70; duration: 130193 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 70 00 00 10 D0`.
Payload transmission bits: `1001100010010000000001000000111000000000000000000000100000001011`.
Blocks (including separators): `10011000100100000000010000001110010 | 00000000000000000000100000001011`.
Issues: [].

### Session 2 capture 3

Symbols: 70; duration: 126957 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 50 00 40 00 10`.
Payload transmission bits: `1001100001010000000001000000101000000000000000100000000000001000`.
Blocks (including separators): `10011000010100000000010000001010010 | 00000000000000100000000000001000`.
Issues: [].

### Session 2 capture 4

Symbols: 70; duration: 130168 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 70 00 00 10 E0`.
Payload transmission bits: `1001100001010000000001000000111000000000000000000000100000000111`.
Blocks (including separators): `10011000010100000000010000001110010 | 00000000000000000000100000000111`.
Issues: [].

### Session 2 capture 5

Symbols: 70; duration: 128038 us; structure: [35, 32]; checksum: True.
Payload: `19 0B 20 50 00 40 00 20`.
Payload transmission bits: `1001100011010000000001000000101000000000000000100000000000000100`.
Blocks (including separators): `10011000110100000000010000001010010 | 00000000000000100000000000000100`.
Issues: [].

### Session 2 capture 6

Symbols: 70; duration: 132397 us; structure: [35, 32]; checksum: True.
Payload: `19 0B 20 70 00 00 10 F0`.
Payload transmission bits: `1001100011010000000001000000111000000000000000000000100000001111`.
Blocks (including separators): `10011000110100000000010000001110010 | 00000000000000000000100000001111`.
Issues: [].

### Session 2 capture 7

Symbols: 70; duration: 126929 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 50 00 40 00 10`.
Payload transmission bits: `1001100001010000000001000000101000000000000000100000000000001000`.
Blocks (including separators): `10011000010100000000010000001010010 | 00000000000000100000000000001000`.
Issues: [].

### Session 2 capture 8

Symbols: 70; duration: 130165 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 70 00 00 10 E0`.
Payload transmission bits: `1001100001010000000001000000111000000000000000000000100000000111`.
Blocks (including separators): `10011000010100000000010000001110010 | 00000000000000000000100000000111`.
Issues: [].

### Session 2 capture 9

Symbols: 70; duration: 125819 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 50 00 40 00 00`.
Payload transmission bits: `1001100010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 2 capture 10

Symbols: 70; duration: 130182 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 70 00 00 10 D0`.
Payload transmission bits: `1001100010010000000001000000111000000000000000000000100000001011`.
Blocks (including separators): `10011000100100000000010000001110010 | 00000000000000000000100000001011`.
Issues: [].

### Session 2 capture 11

Symbols: 70; duration: 126958 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 50 00 40 00 10`.
Payload transmission bits: `1001100001010000000001000000101000000000000000100000000000001000`.
Blocks (including separators): `10011000010100000000010000001010010 | 00000000000000100000000000001000`.
Issues: [].

### Session 2 capture 12

Symbols: 70; duration: 130162 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 70 00 00 10 E0`.
Payload transmission bits: `1001100001010000000001000000111000000000000000000000100000000111`.
Blocks (including separators): `10011000010100000000010000001110010 | 00000000000000000000100000000111`.
Issues: [].

### Session 2 capture 13

Symbols: 70; duration: 128042 us; structure: [35, 32]; checksum: True.
Payload: `19 0B 20 50 00 40 00 20`.
Payload transmission bits: `1001100011010000000001000000101000000000000000100000000000000100`.
Blocks (including separators): `10011000110100000000010000001010010 | 00000000000000100000000000000100`.
Issues: [].

### Session 2 capture 14

Symbols: 70; duration: 132404 us; structure: [35, 32]; checksum: True.
Payload: `19 0B 20 70 00 00 10 F0`.
Payload transmission bits: `1001100011010000000001000000111000000000000000000000100000001111`.
Blocks (including separators): `10011000110100000000010000001110010 | 00000000000000000000100000001111`.
Issues: [].

### Session 2 capture 15

Symbols: 70; duration: 126935 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 50 00 40 00 10`.
Payload transmission bits: `1001100001010000000001000000101000000000000000100000000000001000`.
Blocks (including separators): `10011000010100000000010000001010010 | 00000000000000100000000000001000`.
Issues: [].

### Session 2 capture 16

Symbols: 70; duration: 130201 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 70 00 00 10 E0`.
Payload transmission bits: `1001100001010000000001000000111000000000000000000000100000000111`.
Blocks (including separators): `10011000010100000000010000001110010 | 00000000000000000000100000000111`.
Issues: [].

### Session 2 capture 17

Symbols: 70; duration: 125820 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 50 00 40 00 00`.
Payload transmission bits: `1001100010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 2 capture 18

Symbols: 70; duration: 130140 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 70 00 00 10 D0`.
Payload transmission bits: `1001100010010000000001000000111000000000000000000000100000001011`.
Blocks (including separators): `10011000100100000000010000001110010 | 00000000000000000000100000001011`.
Issues: [].

### Session 2 capture 19

Symbols: 70; duration: 126947 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 50 00 40 00 10`.
Payload transmission bits: `1001100001010000000001000000101000000000000000100000000000001000`.
Blocks (including separators): `10011000010100000000010000001010010 | 00000000000000100000000000001000`.
Issues: [].

### Session 2 capture 20

Symbols: 70; duration: 130162 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 70 00 00 10 E0`.
Payload transmission bits: `1001100001010000000001000000111000000000000000000000100000000111`.
Blocks (including separators): `10011000010100000000010000001110010 | 00000000000000000000100000000111`.
Issues: [].

### Session 2 capture 21

Symbols: 70; duration: 128061 us; structure: [35, 32]; checksum: True.
Payload: `19 0B 20 50 00 40 00 20`.
Payload transmission bits: `1001100011010000000001000000101000000000000000100000000000000100`.
Blocks (including separators): `10011000110100000000010000001010010 | 00000000000000100000000000000100`.
Issues: [].

### Session 2 capture 22

Symbols: 70; duration: 132393 us; structure: [35, 32]; checksum: True.
Payload: `19 0B 20 70 00 00 10 F0`.
Payload transmission bits: `1001100011010000000001000000111000000000000000000000100000001111`.
Blocks (including separators): `10011000110100000000010000001110010 | 00000000000000000000100000001111`.
Issues: [].

### Session 2 capture 23

Symbols: 70; duration: 126926 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 50 00 40 00 10`.
Payload transmission bits: `1001100001010000000001000000101000000000000000100000000000001000`.
Blocks (including separators): `10011000010100000000010000001010010 | 00000000000000100000000000001000`.
Issues: [].

### Session 2 capture 24

Symbols: 70; duration: 130167 us; structure: [35, 32]; checksum: True.
Payload: `19 0A 20 70 00 00 10 E0`.
Payload transmission bits: `1001100001010000000001000000111000000000000000000000100000000111`.
Blocks (including separators): `10011000010100000000010000001110010 | 00000000000000000000100000000111`.
Issues: [].

### Session 2 capture 25

Symbols: 70; duration: 125817 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 50 00 40 00 00`.
Payload transmission bits: `1001100010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 2 capture 26

Symbols: 70; duration: 130160 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 70 00 00 10 D0`.
Payload transmission bits: `1001100010010000000001000000111000000000000000000000100000001011`.
Blocks (including separators): `10011000100100000000010000001110010 | 00000000000000000000100000001011`.
Issues: [].

### Session 2 capture 27

Symbols: 70; duration: 125825 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 50 00 40 00 80`.
Payload transmission bits: `1000100010010000000001000000101000000000000000100000000000000001`.
Blocks (including separators): `10001000100100000000010000001010010 | 00000000000000100000000000000001`.
Issues: [].

### Session 2 capture 28

Symbols: 70; duration: 127979 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 70 00 00 10 50`.
Payload transmission bits: `1000100010010000000001000000111000000000000000000000100000001010`.
Blocks (including separators): `10001000100100000000010000001110010 | 00000000000000000000100000001010`.
Issues: [].

### Session 3 capture 1

Symbols: 70; duration: 125823 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 50 00 40 00 00`.
Payload transmission bits: `1001100010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 2

Symbols: 70; duration: 130134 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 70 00 00 10 D0`.
Payload transmission bits: `1001100010010000000001000000111000000000000000000000100000001011`.
Blocks (including separators): `10011000100100000000010000001110010 | 00000000000000000000100000001011`.
Issues: [].

### Session 3 capture 3

Symbols: 70; duration: 125850 us; structure: [35, 32]; checksum: True.
Payload: `29 09 20 50 00 40 00 00`.
Payload transmission bits: `1001010010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10010100100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 4

Symbols: 70; duration: 130159 us; structure: [35, 32]; checksum: True.
Payload: `29 09 20 70 00 00 20 E0`.
Payload transmission bits: `1001010010010000000001000000111000000000000000000000010000000111`.
Blocks (including separators): `10010100100100000000010000001110010 | 00000000000000000000010000000111`.
Issues: [].

### Session 3 capture 5

Symbols: 70; duration: 126928 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 50 00 40 00 00`.
Payload transmission bits: `1001110010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011100100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 6

Symbols: 70; duration: 133449 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 70 00 00 30 F0`.
Payload transmission bits: `1001110010010000000001000000111000000000000000000000110000001111`.
Blocks (including separators): `10011100100100000000010000001110010 | 00000000000000000000110000001111`.
Issues: [].

### Session 3 capture 7

Symbols: 70; duration: 126890 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 50 00 40 00 00`.
Payload transmission bits: `1001110010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011100100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 8

Symbols: 70; duration: 127946 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 70 00 00 40 00`.
Payload transmission bits: `1001110010010000000001000000111000000000000000000000001000000000`.
Blocks (including separators): `10011100100100000000010000001110010 | 00000000000000000000001000000000`.
Issues: [].

### Session 3 capture 9

Symbols: 70; duration: 126929 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 50 00 40 00 00`.
Payload transmission bits: `1001110010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011100100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 10

Symbols: 70; duration: 130156 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 70 00 00 50 10`.
Payload transmission bits: `1001110010010000000001000000111000000000000000000000101000001000`.
Blocks (including separators): `10011100100100000000010000001110010 | 00000000000000000000101000001000`.
Issues: [].

### Session 3 capture 11

Symbols: 70; duration: 124719 us; structure: [35, 32]; checksum: True.
Payload: `09 09 20 50 00 40 00 00`.
Payload transmission bits: `1001000010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10010000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 12

Symbols: 70; duration: 126871 us; structure: [35, 32]; checksum: True.
Payload: `09 09 20 70 00 00 00 C0`.
Payload transmission bits: `1001000010010000000001000000111000000000000000000000000000000011`.
Blocks (including separators): `10010000100100000000010000001110010 | 00000000000000000000000000000011`.
Issues: [].

### Session 3 capture 13

Symbols: 70; duration: 125824 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 50 00 40 00 00`.
Payload transmission bits: `1001100010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 14

Symbols: 70; duration: 130132 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 70 00 00 10 D0`.
Payload transmission bits: `1001100010010000000001000000111000000000000000000000100000001011`.
Blocks (including separators): `10011000100100000000010000001110010 | 00000000000000000000100000001011`.
Issues: [].

### Session 3 capture 15

Symbols: 70; duration: 125813 us; structure: [35, 32]; checksum: True.
Payload: `29 09 20 50 00 40 00 00`.
Payload transmission bits: `1001010010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10010100100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 16

Symbols: 70; duration: 130149 us; structure: [35, 32]; checksum: True.
Payload: `29 09 20 70 00 00 20 E0`.
Payload transmission bits: `1001010010010000000001000000111000000000000000000000010000000111`.
Blocks (including separators): `10010100100100000000010000001110010 | 00000000000000000000010000000111`.
Issues: [].

### Session 3 capture 17

Symbols: 70; duration: 126916 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 50 00 40 00 00`.
Payload transmission bits: `1001110010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011100100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 18

Symbols: 70; duration: 133490 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 70 00 00 30 F0`.
Payload transmission bits: `1001110010010000000001000000111000000000000000000000110000001111`.
Blocks (including separators): `10011100100100000000010000001110010 | 00000000000000000000110000001111`.
Issues: [].

### Session 3 capture 19

Symbols: 70; duration: 126951 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 50 00 40 00 00`.
Payload transmission bits: `1001110010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011100100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 20

Symbols: 70; duration: 128001 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 70 00 00 40 00`.
Payload transmission bits: `1001110010010000000001000000111000000000000000000000001000000000`.
Blocks (including separators): `10011100100100000000010000001110010 | 00000000000000000000001000000000`.
Issues: [].

### Session 3 capture 21

Symbols: 70; duration: 126952 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 50 00 40 00 00`.
Payload transmission bits: `1001110010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011100100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 22

Symbols: 70; duration: 130161 us; structure: [35, 32]; checksum: True.
Payload: `39 09 20 70 00 00 50 10`.
Payload transmission bits: `1001110010010000000001000000111000000000000000000000101000001000`.
Blocks (including separators): `10011100100100000000010000001110010 | 00000000000000000000101000001000`.
Issues: [].

### Session 3 capture 23

Symbols: 70; duration: 124714 us; structure: [35, 32]; checksum: True.
Payload: `09 09 20 50 00 40 00 00`.
Payload transmission bits: `1001000010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10010000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 24

Symbols: 70; duration: 126865 us; structure: [35, 32]; checksum: True.
Payload: `09 09 20 70 00 00 00 C0`.
Payload transmission bits: `1001000010010000000001000000111000000000000000000000000000000011`.
Blocks (including separators): `10010000100100000000010000001110010 | 00000000000000000000000000000011`.
Issues: [].

### Session 3 capture 25

Symbols: 70; duration: 125810 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 50 00 40 00 00`.
Payload transmission bits: `1001100010010000000001000000101000000000000000100000000000000000`.
Blocks (including separators): `10011000100100000000010000001010010 | 00000000000000100000000000000000`.
Issues: [].

### Session 3 capture 26

Symbols: 70; duration: 130152 us; structure: [35, 32]; checksum: True.
Payload: `19 09 20 70 00 00 10 D0`.
Payload transmission bits: `1001100010010000000001000000111000000000000000000000100000001011`.
Blocks (including separators): `10011000100100000000010000001110010 | 00000000000000000000100000001011`.
Issues: [].

### Session 3 capture 27

Symbols: 70; duration: 125813 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 50 00 40 00 80`.
Payload transmission bits: `1000100010010000000001000000101000000000000000100000000000000001`.
Blocks (including separators): `10001000100100000000010000001010010 | 00000000000000100000000000000001`.
Issues: [].

### Session 3 capture 28

Symbols: 70; duration: 127949 us; structure: [35, 32]; checksum: True.
Payload: `11 09 20 70 00 00 10 50`.
Payload transmission bits: `1000100010010000000001000000111000000000000000000000100000001010`.
Blocks (including separators): `10001000100100000000010000001110010 | 00000000000000000000100000001010`.
Issues: [].

## Consecutive operation differences (one-based payload bits)

- Session 1, op 1 -> 2: [[4, 64], [4, 64]]
- Session 1, op 2 -> 3: [[4, 64], [4, 64]]
- Session 1, op 3 -> 4: [[4, 64], [4, 64]]
- Session 1, op 4 -> 5: [[4, 64], [4, 64]]
- Session 1, op 5 -> 6: [[4, 64], [4, 64]]
- Session 2, op 1 -> 2: [[9, 10, 61], [9, 10, 61, 62]]
- Session 2, op 2 -> 3: [[9, 61, 62], [9, 61]]
- Session 2, op 3 -> 4: [[9, 61, 62], [9, 61]]
- Session 2, op 4 -> 5: [[9, 10, 61], [9, 10, 61, 62]]
- Session 2, op 5 -> 6: [[9, 10, 61], [9, 10, 61, 62]]
- Session 2, op 6 -> 7: [[9, 61, 62], [9, 61]]
- Session 2, op 7 -> 8: [[9, 61, 62], [9, 61]]
- Session 2, op 8 -> 9: [[9, 10, 61], [9, 10, 61, 62]]
- Session 2, op 9 -> 10: [[9, 10, 61], [9, 10, 61, 62]]
- Session 2, op 10 -> 11: [[9, 61, 62], [9, 61]]
- Session 2, op 11 -> 12: [[9, 61, 62], [9, 61]]
- Session 2, op 12 -> 13: [[9, 10, 61], [9, 10, 61, 62]]
- Session 2, op 13 -> 14: [[4, 64], [4, 64]]
- Session 3, op 1 -> 2: [[5, 6], [5, 6, 53, 54, 61, 62]]
- Session 3, op 2 -> 3: [[5], [5, 53, 61]]
- Session 3, op 3 -> 4: [[], [53, 54, 55, 61, 62, 63, 64]]
- Session 3, op 4 -> 5: [[], [53, 61]]
- Session 3, op 5 -> 6: [[5, 6], [5, 6, 53, 55, 61, 63, 64]]
- Session 3, op 6 -> 7: [[5], [5, 53, 61]]
- Session 3, op 7 -> 8: [[5, 6], [5, 6, 53, 54, 61, 62]]
- Session 3, op 8 -> 9: [[5], [5, 53, 61]]
- Session 3, op 9 -> 10: [[], [53, 54, 55, 61, 62, 63, 64]]
- Session 3, op 10 -> 11: [[], [53, 61]]
- Session 3, op 11 -> 12: [[5, 6], [5, 6, 53, 55, 61, 63, 64]]
- Session 3, op 12 -> 13: [[5], [5, 53, 61]]
- Session 3, op 13 -> 14: [[4, 64], [4, 64]]

## Input issues

- session 1: capture IDs jump 15 -> 17
- session 1: 8 operations, 6 annotations; excess operations remain unlabelled
- session 3: 14 operations, 13 annotations; excess operations remain unlabelled
