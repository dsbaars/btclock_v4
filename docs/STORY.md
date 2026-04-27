# BTClock — Project story

> Posted by **Djuri** on Nostr, **2024-11-18**. Reproduced verbatim with the
> author's permission. Original notes:
> [primal.net](https://primal.net/e/nevent1qqst22qclvkw7p8fqhcukrcc6m8unjh0ppp72t9heq8gf89eeu82qag28s6wf)
> · [njump.me](https://njump.me/nevent1qqst22qclvkw7p8fqhcukrcc6m8unjh0ppp72t9heq8gf89eeu82qag28s6wf).

OK, past few days were quite wild. I am currenly in El Salvador because
I was attending the Adopting Bitcoin conference. My very limited
internet connection here made it quite difficult to engage, I mostly
just got push notifications. Hereby I want to take the opportunity to
tell the story from my side.

**TL;DR:** We were just having fun, we thought it was okay. Apperently
it's not and nvk is is so upset that he felt he had to file a GitHub
takedown request. I feel that's not how bitcoiners behave.

## How it started

I have been working on the **#BTClock** since April '23. It started of
some sort of bet where I challenged myself to make a BTC ticker as
cheap as possible. I just ordered seven eInk displays on AliExpress
with the intention of showing information related to bitcoin using an
ESP32. Why seven? Because the words **BITCOIN** and **SATOSHI** both
consist of seven letters and connecting more screens to the ESP32 did
not seem possible without adding I/O-port expanders.

Yes. The idea was inspired (among other projects) by Blockclock, which
also uses eInk displays. I don't own one, so I couldn't "reverse
engineer" or use any direct inspiration of it but the way you can
combine multiple eInk displays to display information in an useful way
is very limited.

After getting the schematic right. I created a 3D printable case for
it ([thingiverse.com/thing:6009628](https://www.thingiverse.com/thing:6009628))
and named it BTClock after someone's idea in the Dutch Noderunners
community. Since it consists of seven characters I thought it's the
perfect name since it fits the seven displays too.

> The first version of the #BTClock — #ThrowbackThursday

## May 2023

This allows you to create a BTClock for ±70 euro (if you own a 3D
printer). I shared the progress of the project in the MakerBits
telegram group. Ben Arc checked with nvk how he felt about it, and I
have seen his responsse where he replied with that he felt honored and
that I should go ahead. In hindsight, I should've asked Ben to forward
this to me but I wouldn't expect this to "haunt" me like it did last
week.

After sharing the very spaghetti-y schematic in some Telegram groups,
someone replied that he was working on making an actual PCB out of it.
After some DM'ing back and forth we got our first PCB prototype that
needed some work. After 4 iterations we finally got it how we wanted
and were able to sell DIY kits. The PCB is real artwork, so we wanted
to show it off. Therefore we added a acrylic backplate to be able to
protect it while still able to see the proof-of-work by Madbo, the PCB
artwork designer. We used a lot of through hole components to make it
accesible for anyone who has a soldering iron.

## August 2023

In August 2023 we finally got the kits ready, while the initial plan
was to only sell the DIY kit within the Dutch Noderunners community, I
brought smoe with me to Baltic Honeybadger '23 as well, where it
proved to be pretty succesful while sitting next to Ben in the
chilling lounge.

I got a lot of encouraging and positive remarks about the project,
including Daniel Prince ([@princeysov](https://x.com/Princey21M)) who
[tweeted about this](https://x.com/Princey21M/status/1698257536418267624).

It appeared that this tweet firstly poked the lion, since nvk sent
some of his Coinkite employees to me after this tweet, a group of 4
people asked a lot of questions while filming me (without my consent,
I didn't realize this when they were asking it but when they walked
away it was clear to see the iPhone was recording video).

Not a lot later, nvk sent out
[a tweet](https://x.com/nvk/status/1698423362819682400) which was
clearly about the BTClock project without explicitly mentioning the
name. However, in my opinion this did still fit the message I read he
wrote to Ben that he was more flattered than upset. It was only then
when I learned about how he reacted to Foundation Passport and how he
behaves regarding the SeedSigner project.

## March 2024

During Bitcoin Atlantis in March 2024, there was a nice space for the
MakerBits projects including the BTClock; the software by this time
has many improvements, including real-time new-block notifications by
utilizing the Mempool.space Websocket API. Since the hardware is also
open source, many alternative faceplates are available including a
stainless steel one and a completely white one. Multiple people
witnessed nvk walking passed the maker space while looking at me, but
he did not engage or whatsoever. I would expect if he would take
offense by the product, he would've approached me by now but he
didn't. He does know who I am though, both in real-life and I'm
posting enough on my social media accounts (Nostr and Xitter) to find
me there as well.

## Fast forward to November 2024

A lot has happened in the development of the BTClock, among other
things I added a BitAxe integration and Nostr Zap Notifications to the
software and we managed to add frontlight to the eInk displays which I
really think sets the BTClock apart from other "tickers" like the
Blockclock mini. The Blockclock mini hasn't had any updates for two
years (since October 3rd, 2022) which seems to be like the Blockclock
mini can be considered end-of-life.

I therefore was very suprised that when I woke up on November 14th, I
received an e-mail that the BTClock GitHub organization was flagged
because of alleged trademark infringement of the BTClock. This was
according to a report by Coinkite to GitHub. The letter I shared in
the MakerBits Telegram group was ONLY sent to GitHub, not to me
(although I haven't been home for a while so I don't know something is
waiting in my physical mailbox).

Luckily git is decentralized version control and there are already
mirrors of the hardware and software source available. Also GitHub
allows me to appeal but haven't taken the chance to do so.

Although I do understand that the concept of trademarks exists for a
reason, I feel this a very cowardly move and certainly not something
that I would expect from fellow bitcoiners. Just sending me a DM on
Nostr, X or Telegram is the least I would expect. I don't mean to
offend or infringe, and would be happy to discuss options if open to
it. Either in DM or out in the open.

I'm all about transparency so NVK (npub1az9…m8y8) here is my
invitation to discuss this out in the open. I'm sorry that we seem to
upset you. what is it that bothers you, can we find a way to let the
projects co-exist? Or even see if the BTClock software can adapted to
your Blockclock mini hardware.

---

## What happened next

The Forgejo instance at [git.btclock.dev](https://git.btclock.dev/btclock)
became the canonical home of the project: hardware and firmware
sources, the v3 Arduino history, the v4 ESP-IDF rewrite this site
documents, and the build that drives the device on your desk today.
The community kept shipping — DIY kits, alternative faceplates, the
WASM doc renderer, frontlights, mining-pool integrations, Nostr zap
overlays, and a multilingual quickstart — and the project is still
open hardware, open firmware, open data feeds.
