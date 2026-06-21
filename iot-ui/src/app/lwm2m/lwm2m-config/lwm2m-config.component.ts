import { Component, Input, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { DataStoreService } from '../../../common/datastore.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';

@Component({
  selector: 'app-lwm2m-config',
  templateUrl: './lwm2m-config.component.html',
  styleUrls: ['./lwm2m-config.component.scss']
})
export class Lwm2mConfigComponent implements OnInit, OnDestroy {
  // Which tab is rendering this component: 'server' shows the registration
  // fields (Server Object), 'security' shows serial + Bootstrap PSK
  // (Security Object). Same form-group backs both; we just show a subset.
  @Input() mode: 'server' | 'security' = 'server';

  serverForm: FormGroup;
  loading = true; saving = false; msg = '';

  // PSK provisioning (task H).
  devMode = false;             // iot.dev.mode — gates PSK generation/reveal
  serialAutoDetected = false;  // true when iot.serial was already populated (RPi)
  generatedPsk = '';           // last generated BS PSK (hex), shown once to copy
  generatingPsk = false;

  // Third-party BS PSK override: when on, the device uses an operator-supplied
  // identity + key (verbatim) for the bootstrap handshake instead of the
  // sha256(endpoint) derivation. The DM credential path is unaffected.
  bsOverride = false;          // iot.bs.psk.override
  savingOverride = false;

  private sub = new Subscription();
  private readonly CFG_KEYS = [
    'iot.serial', 'iot.dev.mode', 'iot.bs.uri', 'iot.binding', 'iot.lifetime',
  ];

  get isAdmin(): boolean { return this.session.isAdmin; }

  /** True while the override checkbox is ticked (drives the extra fields). */
  get overrideOn(): boolean { return this.serverForm.get('bs_override')?.value === true; }

  constructor(private http: HttpsvcService, private ds: DataStoreService, fb: FormBuilder, private session: SessionService, private toast: ToastService) {
    this.serverForm = fb.group({
      serial:     [''],
      bs_uri:     ['coaps://'],
      // Third-party BS PSK override (commissioning).
      bs_override: [false],
      bs_identity: [''],
      bs_key:      [''],
      // Bootstrap-provided (read-only on the Server tab).
      server_uri: [''],
      binding:    ['U'],
      lifetime:   [86400],
    });
  }

  ngOnInit(): void {
    // Paint instantly from the shared prefetched cache (no per-page round-trip),
    // then stay live off the appglobal store.
    this.applyData(this.ds.snapshot());
    this.loading = false;
    // The Security tab is an editable form; re-apply the commissioning fields
    // only while pristine so a late prefetch fills them without clobbering
    // in-progress edits. These keys aren't republished by /status, so they
    // fire once when the prefetch lands.
    for (const k of this.CFG_KEYS)
      this.sub.add(this.ds.observe(k).subscribe(() => {
        if (!this.serverForm.dirty) this.applyData(this.ds.snapshot());
      }));

    // The Server (Device Management) tab is read-only STATUS — the DM URI is
    // pushed by the bootstrap server after the client registers. Keep it live
    // off the shared store (single /status long-poll) so it never shows a
    // stale "(set by bootstrap)" once iot.dm.uri is written.
    if (this.mode === 'server') {
      this.sub.add(this.ds.observe('iot.dm.uri').subscribe(() => this.refreshDmUri()));
      this.sub.add(this.ds.observe('iot.server.uri').subscribe(() => this.refreshDmUri()));
    }

    // Security tab: hydrate the third-party BS PSK override toggle + identity
    // from the data-store (gid:engineer keys, readable to httpd under dev-mode).
    // The key itself is write-only and never read back.
    if (this.mode === 'security' && this.isAdmin) this.loadBsOverride();
  }

  /** Load the current override flag + custom identity (never the key). */
  private loadBsOverride(): void {
    this.http.dbGet(['iot.bs.psk.override', 'iot.bs.psk.identity']).subscribe({
      next: (r) => {
        if (!r.ok || !r.data) return;
        this.bsOverride = r.data['iot.bs.psk.override'] === true;
        // Only adopt the stored identity while the form is pristine, so a late
        // response never clobbers an in-progress edit.
        if (!this.serverForm.get('bs_identity')?.dirty) {
          this.serverForm.patchValue({
            bs_override: this.bsOverride,
            bs_identity: (r.data['iot.bs.psk.identity'] as string) || '',
          });
        }
      },
      error: () => { /* keep defaults */ }
    });
  }

  private applyData(d: Record<string, unknown>): void {
    const serial = (d['iot.serial'] as string) || '';
    // A serial already in the store means the RPi client auto-filled it →
    // present it read-only. Empty → installer enters it.
    this.serialAutoDetected = serial.length > 0;
    this.devMode = d['iot.dev.mode'] === true;
    this.serverForm.patchValue({
      serial:     serial,
      bs_uri:     d['iot.bs.uri']    || 'coaps://',
      // DM Server URI: prefer the actual bootstrap-delivered URI the client
      // persisted (iot.dm.uri); fall back to the legacy ds-driven server.uri,
      // then to the placeholder when both empty.
      server_uri: (d['iot.dm.uri'] as string)
                  || (d['iot.server.uri'] as string)
                  || '(set by bootstrap)',
      binding:    d['iot.binding']    || 'U',
      lifetime:   d['iot.lifetime']   || 86400,
    });
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  private refreshDmUri(): void {
    const dm = this.ds.getString('iot.dm.uri');
    const legacy = this.ds.getString('iot.server.uri');
    this.serverForm.patchValue({ server_uri: dm || legacy || '(set by bootstrap)' });
  }

  // Only the Security (commissioning) tab saves — the Server tab is
  // read-only DM status pushed by the bootstrap server. Writes the
  // serial + bootstrap URI (gid:engineer, allowed in commissioning mode).
  save(): void {
    this.saving = true; this.msg = '';
    const v = this.serverForm.value;
    const pairs: { key: string; value: unknown }[] = [
      { key: 'iot.bs.uri', value: v.bs_uri },
    ];
    if (!this.serialAutoDetected && v.serial) {
      pairs.push({ key: 'iot.serial', value: v.serial });
    }
    this.ds.write(pairs).subscribe({
      next: (r) => { this.saving = false; if(r.ok) this.toast.success('Commissioning saved'); else this.toast.error(r.err||'Save failed'); },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }

  /**
   * Generate a 16-byte (128-bit) BS PSK in the browser (never leaves the
   * device un-encrypted), show it once for the engineer to copy into cloud-ui,
   * and store it write-only. 128-bit matches tinydtls' AES-128-CCM PSK key
   * length and the 128-bit derived identity. Only available in dev-mode
   * (commissioning), where the data-store bypasses the PSK ACLs.
   */
  generateBsPsk(): void {
    if (!this.devMode || !this.isAdmin) return;
    const bytes = new Uint8Array(16);
    crypto.getRandomValues(bytes);
    const hex = Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
    this.generatedPsk = hex;
    this.generatingPsk = true;
    // Only the secret is commissioned. The BS PSK identity is DERIVED as
    // sha256(endpoint) by both the device and the cloud, so we don't store it.
    this.http.dbSet([
      { key: 'iot.bs.psk.key', value: hex },
    ]).subscribe({
      next: (r) => {
        this.generatingPsk = false;
        if (r.ok) this.toast.success('BS PSK generated — copy serial + key into cloud-ui provisioning');
        else this.toast.error(r.err || 'PSK store failed');
      },
      error: () => { this.generatingPsk = false; this.toast.error('PSK store failed'); }
    });
  }

  /**
   * Persist the third-party BS PSK override. Writes the override flag and (when
   * enabled) the operator-supplied identity. The hex key is written only when a
   * new value was typed, so re-saving without re-entering it leaves the stored
   * secret intact (write-only). The client must be restarted to pick the new
   * credentials up. Requires commissioning mode.
   */
  saveBsOverride(): void {
    if (!this.devMode || !this.isAdmin) return;
    const on = this.overrideOn;
    const identity = (this.serverForm.get('bs_identity')?.value || '').trim();
    const key = (this.serverForm.get('bs_key')?.value || '').trim();

    if (on && !identity) { this.toast.error('Enter the bootstrap PSK identity'); return; }
    if (key && !/^[0-9a-fA-F]+$/.test(key)) {
      this.toast.error('PSK key must be hex (0-9, a-f)'); return;
    }
    if (key && key.length % 2 !== 0) {
      this.toast.error('PSK key hex length must be even'); return;
    }

    const pairs: { key: string; value: unknown }[] = [
      { key: 'iot.bs.psk.override', value: on },
    ];
    if (on) {
      pairs.push({ key: 'iot.bs.psk.identity', value: identity });
      if (key) pairs.push({ key: 'iot.bs.psk.key', value: key.toLowerCase() });
    }

    this.savingOverride = true;
    this.http.dbSet(pairs).subscribe({
      next: (r) => {
        this.savingOverride = false;
        if (r.ok) {
          this.bsOverride = on;
          this.serverForm.get('bs_key')?.reset('');   // never retain the secret in the form
          this.toast.success(on
            ? 'Custom bootstrap PSK saved — client restarting to re-establish DTLS'
            : 'Custom bootstrap PSK disabled — client restarting (derived identity)');
        } else {
          this.toast.error(r.err || 'Save failed');
        }
      },
      error: () => { this.savingOverride = false; this.toast.error('Save failed'); }
    });
  }
}
