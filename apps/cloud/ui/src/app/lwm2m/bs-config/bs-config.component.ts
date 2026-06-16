import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { FormBuilder, FormGroup } from '@angular/forms';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';
import { DataStoreService } from '../../../common/datastore.service';

@Component({
  selector: 'app-bs-config',
  templateUrl: './bs-config.component.html',
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 8px 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 0 0 16px 0; font-size: 14px; font-weight: 600;
         border-bottom: 1px solid #e0e0e0; padding-bottom: 8px; }
    .hint { color: #888; font-weight: normal; font-size: 11px; }
  `]
})
export class BsConfigComponent implements OnInit, OnDestroy {
  bsForm: FormGroup;
  loading = true;
  savingBs = false;
  private sub = new Subscription();
  private readonly KEYS = ['cloud.bs.uri', 'cloud.dm.uri'];

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(
    fb: FormBuilder,
    private session: SessionService,
    private toast: ToastService,
    private ds: DataStoreService
  ) {
    // Only the keys the bootstrap server actually consumes at /bs:
    // cloud.bs.uri (Security/0 RID0) and cloud.dm.uri (Security/1 RID0).
    // Per-device PSK identity/key are minted per-endpoint into
    // cloud.endpoint.credentials — see the Endpoints page.
    this.bsForm = fb.group({
      bs_uri:        ['coaps://0.0.0.0:5684'],
      dm_uri:        ['coaps://0.0.0.0:5683'],
    });
  }

  ngOnInit(): void {
    // Paint instantly from the shared prefetched cache (no per-page round-trip),
    // then stay live off the appglobal store; re-apply only while the form is
    // pristine so a late prefetch fills the fields without clobbering edits.
    this.applyData(this.ds.snapshot());
    this.loading = false;
    for (const k of this.KEYS)
      this.sub.add(this.ds.observe(k).subscribe(() => {
        if (!this.bsForm.dirty) this.applyData(this.ds.snapshot());
      }));
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  private applyData(d: Record<string, unknown>): void {
    this.bsForm.patchValue({
      bs_uri: d['cloud.bs.uri'] || 'coaps://0.0.0.0:5684',
      dm_uri: d['cloud.dm.uri'] || 'coaps://0.0.0.0:5683',
    });
  }

  saveBs(): void {
    this.savingBs = true;
    const v = this.bsForm.value;
    this.ds.write([
      { key: 'cloud.bs.uri',           value: v.bs_uri },
      { key: 'cloud.dm.uri',           value: v.dm_uri },
    ]).subscribe({
      next: (r) => {
        this.savingBs = false;
        if (r.ok) this.toast.success('BS config saved');
        else this.toast.error(r.err || 'Save failed');
      },
      error: () => { this.savingBs = false; this.toast.error('Save failed'); }
    });
  }
}
