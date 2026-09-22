# Grok TTS redistribution

Date: 2026-09-22. Ticket: [peilinok/winaudio#89](https://github.com/peilinok/winaudio/issues/89). Parent map: [peilinok/winaudio#86](https://github.com/peilinok/winaudio/issues/86).

Question: do the terms that govern Grok text-to-speech allow this public project to commit the generated WAV files into peilinok/winaudio and ship them inside public release zips to end users? The clips would be short English channel names (LFE, "channel N"), one voice, 48 kHz 16-bit mono. No audio was generated for this note, and no voice was chosen.

Pages were read on 2026-09-22. The contracting party on the current legal pages is SpaceXAI LLC. The same pages are what this ticket calls the xAI terms. They are published at `x.ai`. There is no separate document titled as voice terms or as a text-to-speech output license.

## What governs an API speech file

The consumer terms say the Enterprise Terms govern developer and business use, including the APIs. [Terms of Service — Consumer](https://x.ai/legal/terms-of-service) (last updated September 11, 2026): "Our Enterprise Terms of Service govern the use of our Services for developers and businesses, including SpaceXAI APIs and PromptIDE."

A WAV committed from the text-to-speech API is therefore governed by [Terms of Service — Enterprise](https://x.ai/legal/terms-of-service-enterprise) (last updated August 14, 2026). That agreement incorporates the [Acceptable Use Policy](https://x.ai/legal/acceptable-use-policy) (effective August 14, 2026) by reference (Enterprise §2). It also points at the technical documentation at [https://docs.x.ai/docs](https://docs.x.ai/docs) as Documentation (Enterprise §1.1). The text-to-speech page in that documentation, [Text to Speech](https://docs.x.ai/developers/model-capabilities/audio/text-to-speech) (last updated September 19, 2026), describes `POST /v1/tts` and says the response body is raw audio bytes. It does not state a license, a redistribution right, an attribution duty, or a watermark. The [release notes](https://docs.x.ai/developers/release-notes) say, under March 16 (the March section sits above December 2025, so March 2026): "The Text-to-Speech API is now generally available." The Enterprise beta carve-out (Enterprise §1.4: beta offerings "are not part of the Services and may be subject to additional terms") is not what that documentation says about text-to-speech.

The [legal index](https://x.ai/legal) lists no voice-output terms. It does list a training-data summary, [Public Summary of Training Content for Grok Voice Think Fast 2.0](https://media.x.ai/v1/website/public-summary-of-training-content-grok-voice-think-fast-2.0_29jul2026-0fb8805e.pdf) (version 1, last update 29 July 2026). That PDF identifies the speech-to-speech model and describes training data. The Enterprise Agreement does not incorporate it, and it is not a license for text-to-speech files.

## Ownership is not a redistribution grant

Enterprise §3.1 defines Output as "any response, result, generated content, or other material produced by the Services in response to an Input (excluding system metadata and logs)." A text-to-speech audio body is Output. System metadata and logs are not.

Enterprise §3.2: the customer "owns all right, title, and interest in the Output in perpetuity and, to the fullest extent possible under applicable law, SpaceXAI hereby assigns to Customer all of its right, title, and interest in such Output (but excluding, for clarity, the SpaceXAI Technology)." SpaceXAI Technology, defined in Enterprise §4.1, is the Services, documentation, models, algorithms, and related technology. The assignment does not transfer the voice model. It transfers only whatever interest SpaceXAI has in that Output, and only to the extent the law allows. The sentence does not say a copyright exists in the audio, and it does not say the customer may reproduce the file, commit it to a public repository, or give copies to people who are not the API customer.

Enterprise §1.1 is the grant, and it is narrower than that. During the subscription, the customer may (a) use the Services for its business, (b) use the APIs to build an integration between the Services and the customer's own product (a "Bundled Service"), and (c) "distribute or otherwise make the Bundled Service available" to end users. The same sentence says the customer "may only provide End Users with access to the Services as part of a Bundled Service." A Bundled Service is an integration that gives access to the live Services. A WAV checked into this repository is not access to the Services. The same section then says: "Except for the rights expressly granted under this Agreement, no right, title, or interest of any nature whatsoever is granted, whether by implication, estoppel, reliance, or otherwise."

Enterprise §3.2 also says the customer is "solely responsible for independently evaluating" Output "before relying on or distributing it." Enterprise §9.2 makes the customer indemnify SpaceXAI for "Customer’s use or distribution of Outputs" when that use is alleged to infringe. Those sentences put distribution on the customer. They do not authorize it.

The terms are silent on committing text-to-speech audio to peilinok/winaudio and shipping it in public release zips. Under this ticket's rule, silence does not grant that redistribution. The ownership assignment is not read as the missing grant.

## Attribution

The Enterprise Agreement does not require a credit line on audio Output.

Enterprise §12: "Neither Party may use the other’s name, logos, or marks without the other Party’s written pre-approval in each case (email to suffice). Any permitted use of the other Party’s marks shall comply with the owner’s then-current brand guidelines." A notice that says "Grok" or "SpaceXAI" is use of those marks. It needs written pre-approval first. The brand guidelines apply only after that approval. They are not, by themselves, the output license.

The [Brand Guidelines](https://x.ai/legal/brand-guidelines) (February 14, 2025) say: "If you used Grok to generate text or images, please attribute them to SpaceXAI and Grok by displaying one of the following phrases in a legible and noticeable manner wherever the Grok-generated material is published or distributed: Written with Grok; Created with Grok." The examples are text and images. Audio is not listed. "Please" is not a condition that voids a copy. The same page forbids using the marks in a way that implies endorsement, and forbids using them in an app title or in the name of a product SpaceXAI did not make. That is a trademark rule. It is not permission to ship the audio.

## What would make a shipment a breach

The current terms do not say an unauthorized copy is void. They do prohibit specific uses. A shipment that does any of the following is a breach. Account remedies in the Enterprise Terms and the Acceptable Use Policy include suspension or termination. They are not a copyright-voidness clause.

The customer "will not, and will not permit any third party to" use Output to train foundation models, large language models, or other AI systems, except as an Order Form expressly permits, or "misrepresent that any Output was human-generated" (Enterprise §3.2). Putting the files in a public tree does not, by itself, grant a training right. Licensing them so that a third party may use them to train would be permitting that use. Presenting them as human speech would be the misrepresentation. The terms do not define a caption that must sit on a channel-name file, and they do not say that playback with no claim about origin is a misrepresentation.

The Acceptable Use Policy, incorporated by Enterprise §2, says not to use the Service or Outputs for illegal, harmful, or abusive activities, including:

- "Scraping, harvesting or reselling any Input or Output, or distilling model data or Outputs." The verb that attaches to Output is reselling (and scraping, harvesting, or distilling), not giving a copy away. A free public zip is not, on these words, a resale. The policy does not define reselling to include or exclude a free download, and it does not bless the free download.
- "Modifying, copying, translating, leasing, selling, reselling, distributing, distilling, manipulating, using bots to access, reverse engineer, decompile, disassemble or otherwise seek to obtain the source code of our Service, including our systems, models, or algorithms." "Distributing" in that bullet is aimed at the Service's source code, models, and algorithms. The "or otherwise seek to obtain the source code" close is what the list is doing. It is not a ban on distributing Output. Output is named separately in the reselling bullet above.
- "Using the Service or any Output to develop (or assist anyone in developing) machine learning models or any products or services that compete with SpaceXAI, whether directly or indirectly." Short channel names inside an audio tester are not, on these words, a competing voice service. Using the clips to build one would be.
- "Stripping, altering or circumventing embedded provenance metadata or watermarks." This applies to metadata that is embedded. The text-to-speech documentation cited above does not say the audio bytes contain a watermark or provenance tag. No file was generated here, so this note does not claim the bytes have one or do not.
- "Misleading others or not being transparent regarding your use of AI, including by phishing, creating fake accounts, providing services that appear to be from you, when they are in fact from SpaceXAI, or providing services that appear to originate from SpaceXAI, when they do not." The examples are deception about who is providing a service. This is not an attribution clause for a WAV in a zip. It does not add a required credit line the Enterprise Agreement omits.

## Sources that do not grant it

The consumer terms do not govern the API. If the clips were instead made in consumer Grok, those terms still would not grant this shipment. [Terms of Service — Consumer](https://x.ai/legal/terms-of-service) §4: "When using Output or SpaceXAI’s name, logos, trademarks, or other brand elements, you are required to obtain our permission and attribute your generation of the Output to the Service, as detailed in our Brand Guidelines." The same section's disclosure sentence says the user may be required to apply a disclosure that the content was generated or altered by artificial intelligence. Permission is a further step the terms do not supply. Consumer Output also excludes Grokipedia. The Grokipedia sentence is the only place these terms point at the [Grok-2 community license](https://huggingface.co/xai-org/grok-2/blob/main/LICENSE). That license is not the license for text-to-speech audio.

The [Consumer FAQs](https://x.ai/legal/faq) (May 12, 2025) say an individual user of the Grok app or grok.com is "free to use Grok’s Outputs (including generated images) from your conversations as you wish, including for commercial use," and they ask for brand-guideline attribution. The page says it is for those individual users. The consumer terms call the FAQs a helpful resource. They do not incorporate the FAQs. The consumer entire-agreement clause is the terms plus additional written agreements, and it excludes statements by employees. The FAQ is not a grant for API audio, and "including generated images" does not mention audio.

The [Enterprise FAQs](https://x.ai/legal/faq-enterprise) (last updated February 25, 2025) say "You own the Inputs and Outputs" and "We do ask that you attribute the generated work to Grok as provided in SpaceXAI’s Brand Guidelines." The same answer says the use rights are "pursuant to SpaceXAI’s Enterprise Terms." Enterprise §13.10 says the Agreement is the complete and exclusive statement and supersedes prior and contemporaneous discussions. The Agreement incorporates the Acceptable Use Policy and the Data Processing Addendum. It does not incorporate this FAQ. The FAQ does not add a redistribution right the Agreement omits, and its attribution request is not a term of the Agreement.

Older terms are not the current contract. The October 29, 2024 Enterprise terms said that if you redistribute materials using the Service, you must be able to edit or delete them and must do so on request ([previous Enterprise terms, October 29, 2024](https://x.ai/legal/terms-of-service-enterprise/previous-2024-10-29)). That sentence is not in the August 14, 2026 Enterprise terms or the September 11, 2026 consumer terms. It is not used here as a grant or as a condition.

The [third-party bot terms](https://x.ai/legal/bot-sharing-terms) (effective August 22, 2026) restrict redistributing a shared bot and its configuration. They are not the terms for text-to-speech audio.

## Answer

**silent**

The Enterprise Terms that govern the text-to-speech API assign Output to the customer and do not grant committing those WAV files to this public repository or shipping them in public release zips. Silence does not grant that redistribution. No attribution string is required for the audio. A credit that uses the Grok or SpaceXAI name needs written pre-approval first. Reselling the clips, passing them off as human speech, or permitting anyone to train on them is prohibited. A free zip is not, on the policy's words, a resale, and it is also not an authorized shipment.
