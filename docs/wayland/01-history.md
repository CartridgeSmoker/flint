# Chapter 1: Why Wayland — A Short History

> **Learning Objectives**
> - Describe why X's 1984 design was brilliant, and why it became a burden.
> - Explain the flaws no patch could fix: tearing by design and zero isolation.
> - Trace the arc from Kristian Høgsberg's 2008 announcement to Wayland as the default.
> - Carry one model forward: clients draw, the compositor assembles, the kernel modesets.

Picture 1984. Your workstation hums, and the real computer lives down the hall. You want to run a program over there and see its windows over here. That is the world X was born into.

X began at MIT's Project Athena that year, though the X11 protocol itself did not ship until 1987. Applications — X *clients* — lived anywhere on the network and sent drawing requests to an X *server* beside your monitor, which drew for them. Network transparency was the point, not a trick. For labs of shared minicomputers, it was brilliance.

If you have ever run `ssh -X` and watched a remote program open a window locally, you have touched that magic. Everything that follows is the world changing underneath it.

## The long drift

By the 1990s, the fast machine and the screen were the *same* box on your desk. But X still acted as if they were far apart. Every scroll and frame was posted as requests — "please draw a rectangle here" — to the server. Fine for flat buttons; absurd for video and 3D. You paid postage on every pixel.

So X grew extensions like an old house grows additions. **RENDER** came because core X lacked alpha compositing, gradients, and anti-aliased text and imaging. **RandR** came because the protocol assumed monitors never changed size, then laptops happened. **COMPOSITE, DAMAGE, FIXES** came because flicker-free desktops needed off-screen buffers the core lacked. **DRI** came in 1998 as a designed direct-rendering path that let 3D programs reach the GPU with kernel arbitration. **XInput** kept being revised because scroll wheels and touchpads did not exist in 1984.

Anyone who hand-edited `xorg.conf` in 2005 remembers the folklore: modelines from forums, guessed driver names, two monitors working on alternate Tuesdays. The foundation stayed 1984. New floors kept getting nailed on top.

## Tearing by design

Here is the flaw you could *see*. Under X, nobody owns the final picture. The server draws whenever any client asks, immediately. If your player updates a window mid-refresh, you see a split — tearing. No rule says "wait for vertical blank." No step assembles the whole screen cleanly.

Compositors like Compiz, Mutter, and KWin tried to fix this as just another X *client*: redirect windows off-screen, re-assemble the desktop themselves. The wobbly windows you remember came from here. Clever, but retrofitted — a guest fighting the server for the final frame. Tearing was not a bug. It was the design.

## Open by design, insecure by inheritance

The flaw you could feel but not name: every X client is trusted equally. Any program can screenshot the screen, read keys typed into another window, steal focus, or grab the keyboard. Screenshot and keylogging are not exploits under X11. They are the API.

In a trusted 1980s lab that was a feature. On a laptop running a browser, a password manager, and a game from last night, it is a nightmare. You would never accept it from your phone.

## 2008: Kristian starts over

In November 2008, Kristian Høgsberg announced Wayland. His diagnosis: clients already rendered for themselves with Cairo, Qt, GTK, and OpenGL, and already rendered 3D directly via DRI. The middleman was all cost, no benefit. Work on the tiny reference compositor Weston started around 2010, with its first release in 2012.

Wayland flipped the jobs: **clients draw themselves** into their own buffers; **the compositor assembles the screen** on every vsync; **the kernel modesets** through KMS and DRM. Under X the server draws and everyone begs. Under Wayland applications draw, the compositor directs, the kernel presents. Authority moves where timing and isolation can be guaranteed.

## Builders, flips, and the long tail

A protocol needs builders. In 2015, development began on **Sway**, a Wayland i3 replacement initially built on wlc, which proved a small team could ship something real. In 2017, Drew DeVault and collaborators started **wlroots**, letting ordinary programmers write a compositor without reinventing the input stack and renderer — and dozens of experiments followed.

Then defaults flipped. Fedora made Wayland the GNOME default in 2016. Ubuntu first defaulted to Wayland in 17.10 (2017), reverted to X for the 18.04 LTS, then re-defaulted to Wayland in 21.04 and kept it for 22.04 LTS. Screen sharing broke loudly along the way, precisely because Wayland refused the old skeleton keys — forcing proper replacements via Desktop Portals and PipeWire.

By the late 2000s toolkits and applications had already moved to client-side rendering with Cairo, Qt, GTK, and OpenGL, so shipping drawing commands over the network no longer saved anything — the economic reason network transparency died. DRI2 and the Present extension were the proximate response, letting locally rendered buffers reach the screen with proper vsync inside X. Later, XWayland kept legacy X clients running while the xdg-shell protocol stabilized window roles, which is why the Fedora and Ubuntu default flips could finally stick.

Three late chapters explain the slow tail. **NVIDIA** held out for its own EGLStreams buffers while everyone else used GBM; its 2021 GBM support gave compositors one path to target. **Explicit sync** replaced driver guesswork about buffer readiness with real fences; the `linux-drm-syncobj` protocol landing in 2024 killed a class of mystery stutter. **HDR and color management** are still in flight — X never had a story for mixed HDR and SDR, and Wayland is building it correctly rather than quickly.

| Year | Event | Why it matters |
| --- | --- | --- |
| 1984 / 1987 | X at MIT; X11 protocol ships | Network transparency, server draws for clients |
| ~2000 | RENDER | Alpha compositing, gradients, and anti-aliased text and imaging as a bolt-on |
| 2004 | COMPOSITE / DAMAGE | Off-screen windows enable compositors |
| 2005 | The xorg.conf years | Modelines, folklore, pain |
| 2006 | Compiz wobbles | Compositing as an X client, retrofitted |
| 2008 | Høgsberg announces Wayland | Clients draw, compositor assembles |
| ~2010 / 2012 | Weston begins / first release | Reference compositor proves the model |
| 2015 | Sway begins (initially on wlc) | A small team ships a real Wayland desktop |
| 2016 | Fedora defaults to Wayland | First big flip sticks |
| 2017 | wlroots begins | Small teams can write compositors |
| 2017–2022 | Ubuntu flips, reverts, re-flips | Wayland default in 17.10, X in 18.04 LTS, Wayland again in 21.04 / 22.04 LTS |
| 2021 | NVIDIA supports GBM | One buffer story |
| 2024 | Explicit sync lands | Fences replace guesswork |
| Ongoing | HDR + color management | In progress |

## A common misconception

> **Misconception: "Wayland is a compositor — the thing that replaced X.Org."**
>
> No. **Wayland is a protocol**: how clients and a compositor talk. Weston, Mutter (GNOME), KWin (KDE), and Sway are compositors that *speak* it. X.Org is a program; Wayland is the language its successors share. "Wayland crashed" almost always means "my compositor crashed."

## The landlord analogy

X is a landlord who insists on cooking every meal in a shared kitchen. You hand over groceries (drawing requests), they cook whenever they like, anyone can taste anyone's soup, and dinner lands mid-table-setting — torn, stolen, snooped.

Wayland hires a building manager instead. You cook in your own flat (clients render their buffers). The manager serves the shared meal exactly on schedule, every vsync, and nobody gets your keys unless you hand them over through a portal. The city — the kernel — owns the streets underneath.

Same building. Different idea of responsibility.

Mapped back to the stack: each application renders its own buffers and the compositor only assembles those finished buffers on vsync without drawing app content itself. Isolation comes from the compositor holding the registry of which client owns which buffer and where each input event goes.

## What to carry forward

Carry the shape, not the dates: a brilliant 1984 idea, twenty years of bolt-ons, two structural flaws, and a restart putting drawing, assembly, and modesetting each where they belong.

Next we climb the stack — hardware, kernel, and userspace — to see where each job lives before you learn the protocol itself. You know *why*. Now you learn *where*.

---

### Recap

- X11's server-draws model fit 1984 terminals, not local GPUs — hence extensions, DRI direct rendering, and `xorg.conf` folklore.
- Tearing and insecurity in X were structural: no owner of presentation timing, every client trusted with screenshot and input powers.
- Wayland (2008) inverted the model — clients draw, compositor assembles on vsync, kernel modesets — winning via wlroots, Fedora and Ubuntu defaults, GBM, and explicit sync, with HDR still landing.

### Exercises

1. **Touch the session type.** On your local machine, run `echo $XDG_SESSION_TYPE` and `loginctl show-session $(loginctl | grep $(whoami) | awk '{print $1}' | head -n 1) -p Type -p Display`. Are *you* on Wayland? What do the two commands agree or disagree on?
2. **See the isolation.** Try screenshotting another app's window using only that app's own tools — no portal, no compositor shortcut. Expect a portal prompt under native Wayland versus success under XWayland: which component said no, and why was that refusal impossible under plain X11?
