import { Component, Input, OnInit } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';

@Component({
  selector: 'app-lwm2m-config',
  templateUrl: './lwm2m-config.component.html',
  styleUrls: ['./lwm2m-config.component.scss']
})
export class Lwm2mConfigComponent implements OnInit {
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

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, fb: FormBuilder, private session: SessionService, private toast: ToastService) {
    this.serverForm = fb.group({
      serial:     [''],
      bs_uri:     ['coaps://'],
      // Bootstrap-provided (read-only on the Server tab).
      server_uri: [''],
      binding:    ['U'],
      lifetime:   [86400],
    });
  }

  ngOnInit(): void {
    this.http.dbGet([
      'iot.serial', 'iot.dev.mode', 'iot.bs.uri',
      'iot.server.uri', 'iot.binding', 'iot.lifetime'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          const serial = (d['iot.serial'] as string) || '';
          // A serial already in the store means the RPi client auto-filled
          // it → present it read-only. Empty → installer enters it.
          this.serialAutoDetected = serial.length > 0;
          this.devMode = d['iot.dev.mode'] === true;
          this.serverForm.patchValue({
            serial:     serial,
            bs_uri:     d['iot.bs.uri']    || 'coaps://',
            // DM config below is pushed by the bootstrap server — read-only.
            server_uri: d['iot.server.uri'] || '(set by bootstrap)',
            binding:    d['iot.binding']    || 'U',
            lifetime:   d['iot.lifetime']   || 86400,
          });
        }
        this.loading = false;
      },
      error: () => { this.loading = false; }
    });
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
    this.http.dbSet(pairs).subscribe({
      next: (r) => { this.saving = false; if(r.ok) this.toast.success('Commissioning saved'); else this.toast.error(r.err||'Save failed'); },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }

  /**
   * Generate a 32-byte BS PSK in the browser (never leaves the device
   * un-encrypted), show it once for the engineer to copy into cloud-ui,
   * and store it write-only. Only available in dev-mode (commissioning),
   * where the data-store bypasses the PSK ACLs so httpd can write it.
   */
  generateBsPsk(): void {
    if (!this.devMode || !this.isAdmin) return;
    const bytes = new Uint8Array(32);
    crypto.getRandomValues(bytes);
    const hex = Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
    this.generatedPsk = hex;
    this.generatingPsk = true;
    const serial = this.serverForm.value.serial || this.serverForm.value.endpoint;
    this.http.dbSet([
      { key: 'iot.bs.psk.key',      value: hex },
      { key: 'iot.bs.psk.identity', value: serial },
    ]).subscribe({
      next: (r) => {
        this.generatingPsk = false;
        if (r.ok) this.toast.success('BS PSK generated — copy it into cloud-ui provisioning');
        else this.toast.error(r.err || 'PSK store failed');
      },
      error: () => { this.generatingPsk = false; this.toast.error('PSK store failed'); }
    });
  }
}
