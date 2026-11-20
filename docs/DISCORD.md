# Set up Discord Rich Presence

Start by installing WatchKit for VLC. [Installation guide](INSTALL.md)

## Connect VLC

Use the public **WatchKit Application ID** below. The Discord desktop app publishes the card to the account you're signed in to. Setup requires only the ID and works without a bot invite or token.

```text
1056962833218293800
```

1. Open the Discord desktop app and sign in.
2. In VLC, open **View → WatchKit Settings**.
3. Paste the WatchKit **Application ID** above. Enter `vlc` in **Large image key** if you want the VLC cone as the fallback cover.
4. Check that **Share playback and pause status** is on and click **Save**.
5. Start a video, then open the **Activity** tab of your full Discord profile.

If the card is missing, open Discord's **User Settings → Activity Privacy** and enable activity sharing. Verify the saved Application ID in WatchKit Settings.

The card heading comes from the name you registered in the Developer Portal. The episode and timing lines update as the video plays.

**NumPad `*`** switches publication on or off. On pause, Discord shows a small pause badge on the cover. Hover over the badge for `Paused`. Discord controls how this activity is ranked alongside games and other apps. If the desktop client is closed, the addon reconnects during active playback when Discord becomes available.

## Use WatchKit in both VLC and Chrome

1. If VLC is already set up, open **Settings → Discord → Import ID from WatchKit for VLC** in Chrome WatchKit. Both editions must run under the same Windows account for the import. If Chrome is already set up, copy its Application ID into **View → WatchKit Settings** in VLC.
2. Enable sharing in VLC and Chrome. In Chrome, grant access to the site you want to share and install the Windows bridge from the same build as the extension. VLC connects to Discord directly from the plugin. [WatchKit for Chrome setup](https://github.com/smilekritik/browser-watchkit#connect-discord-in-chrome)
3. Play media in both apps. VLC's playback takes priority while it plays. When VLC pauses or stops, playing browser media takes over. When both have paused media, VLC keeps the card. This handoff requires VLC WatchKit 1.1.3 or later and Chrome WatchKit 1.5.1 or later.

## Troubleshooting

- **No card.** Keep the Discord desktop client signed in, check activity sharing, and verify the public Application ID in VLC. Start playback and check your profile. Discord's browser client can display the card once the desktop client publishes it.
- **No artwork.** Test the connection with the image key blank, then check that the key belongs to the same application. A new asset can take time to appear.
- **A different activity appears first.** Discord decides which activity to display alongside games and streams. Check your full profile for the VLC activity.

[Discord's direct Rich Presence documentation](https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence#rich-presence-without-authentication)

## Playback information

The first line below the application name shows the title and episode label when available. During playback at `1×`, media with a known duration uses Discord's progress bar in the full profile's **Activity** tab. Discord may omit the bar in its compact voice-channel popup. At a different speed, the card shows the player's position and duration followed by the speed, for example `01:47 / 03:06 · Speed 1.5×`. A paused card shows a fixed position. During playback, the addon refreshes the card about every five seconds and sends changes from seeking, pausing, or changing the speed as soon as the Discord update budget allows.

Hover over the large cover in the Discord profile to see the estimated time left at the current speed. VLC also shows it in the regular and fullscreen controls and in the playback overlay opened with **T**. For browser media, the estimate also appears in the WatchKit popup.

## Show preview images

To share frames automatically, enter your uploader's URL in **Video frame upload URL**. WatchKit takes the current frame from VLC's video output during local video playback, at most once every 60 seconds. It sends a proportional JPEG with a longest side of 512 pixels. On pause the card keeps the last frame.

The uploader must accept an HTTPS `POST` with a raw `image/jpeg` body and return a successful response containing only the public HTTPS image URL as text. Serve a distinct image URL for each frame so Discord can fetch the new image. WatchKit has no built-in upload service. Leave the field blank to disable uploads. Only configure a server where you want to send frames from your local videos.

To rotate preview images, put an optional UTF-8 text file next to the video. For `Episode.mp4`, name it `Episode.mp4.discord-previews.txt`. List the video minute (starting at zero) followed by a space and a Discord asset key or a public HTTPS image URL:

```text
0 opening_scene
1 https://your-image-host.example/episode/minute-01.jpg
2 https://your-image-host.example/episode/minute-02.jpg
```

Use stills you have permission to share. You supply and host the images. The addon reads the list once per video and uses the configured VLC icon for minutes without an image. Review the URLs before enabling Discord sharing for a video you received from someone else.

## Get a Discord Application ID

Create your own application if you want a custom name or artwork on your activity.

1. Open the [Discord Developer Portal](https://discord.com/developers/applications) and choose **New Application**.
2. Enter the name you want Discord to show and click **Create**.
3. In **General Information**, copy the public **Application ID**. Enter it in [WatchKit Settings](#connect-vlc) in place of the shared ID.
4. For fallback artwork, upload your image under **Rich Presence → Art Assets**. Enter its key in **Large image key** and save.

![Where to create an application and copy its Application ID](screenshots/discord-application-id.svg)
