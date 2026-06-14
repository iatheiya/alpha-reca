; preamble_template.ll — LLVM IR helper functions for compiled Reca binaries
;
; Placeholders (substituted by gen_compiler.py):
;   {{TOTAL_SIZE}}        — total Aether array size (= AETHER_SIZE = 1M luces)
;   {{ALLOC_PTR_ADDR}}    — address of cursor lux (= K_CURSOR_LUX = 1)
;   {{K_WATERMARK_LUX}}  — address of watermark lux (= K_WATERMARK_LUX = 2)
;
; Canon "Zero is absence":
;   aether[0] = 0 (NULL — the only invariant)
;   aether[K_CURSOR_LUX]    = current bump position (aria convention)
;   aether[K_WATERMARK_LUX] = high-water mark (max addr touched, aria convention)
;
; Lux/lumen layout (aria convention, NOT enforced by Python or LLVM):
;   Data Lux at addr A:  heap[A+0]=word,  heap[A+1]=lumens_head
;   Lumen at addr E:      heap[E+0]=rel,   heap[E+1]=tgt,  heap[E+2]=next_lumen
;
; Functions:
;   @_sys            — syscall dispatcher
;   @reca_alloc(n)   — bump-allocate n luces, update cursor + watermark
;   @reca_link       — add lumen src --rel--> tgt (prepend to src[1])
;   @reca_unlink     — remove first matching lumen from src[1]

define i64 @_sys(i64 %x0, i64 %x1, i64 %x2, i64 %x3, i64 %x8) {
sc_entry:
  switch i64 %x8, label %sc_default [
    i64 64, label %sc_write
    i64 63, label %sc_read
    i64 56, label %sc_openat
    i64 57, label %sc_close
    i64 93, label %sc_exit
  ]
sc_write:
  %sw_buf = alloca i8, i64 %x2
  br label %sw_loop
sw_loop:
  %sw_i = phi i64 [ 0, %sc_write ], [ %sw_inext, %sw_body ]
  %sw_done = icmp eq i64 %sw_i, %x2
  br i1 %sw_done, label %sw_emit, label %sw_body
sw_body:
  %sw_ridx = add i64 %x1, %sw_i
  %sw_rp = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 %sw_ridx
  %sw_rval = load i64, ptr %sw_rp
  %sw_rbyte = trunc i64 %sw_rval to i8
  %sw_wp = getelementptr i8, ptr %sw_buf, i64 %sw_i
  store i8 %sw_rbyte, ptr %sw_wp
  %sw_inext = add i64 %sw_i, 1
  br label %sw_loop
sw_emit:
  %sw_fd = trunc i64 %x0 to i32
  call i64 @write(i32 %sw_fd, ptr %sw_buf, i64 %x2)
  ret i64 0
sc_read:
  %sr_fd = trunc i64 %x0 to i32
  %sr_buf = alloca i8, i64 %x2
  %sr_n = call i64 @read(i32 %sr_fd, ptr %sr_buf, i64 %x2)
  br label %sr_loop
sr_loop:
  %sr_i = phi i64 [ 0, %sc_read ], [ %sr_inext, %sr_body ]
  %sr_done = icmp eq i64 %sr_i, %sr_n
  br i1 %sr_done, label %sr_end, label %sr_body
sr_body:
  %sr_rp = getelementptr i8, ptr %sr_buf, i64 %sr_i
  %sr_byte = load i8, ptr %sr_rp
  %sr_val = zext i8 %sr_byte to i64
  %sr_widx = add i64 %x1, %sr_i
  %sr_wp = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 %sr_widx
  store i64 %sr_val, ptr %sr_wp
  %sr_inext = add i64 %sr_i, 1
  br label %sr_loop
sr_end:
  ret i64 %sr_n
sc_openat:
  %so_pbuf = alloca i8, i64 4096
  br label %so_copy
so_copy:
  %so_ci = phi i64 [ 0, %sc_openat ], [ %so_cinext, %so_cbody ]
  %so_ridx = add i64 %x1, %so_ci
  %so_rp = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 %so_ridx
  %so_rval = load i64, ptr %so_rp
  %so_rbyte = trunc i64 %so_rval to i8
  %so_wp = getelementptr i8, ptr %so_pbuf, i64 %so_ci
  store i8 %so_rbyte, ptr %so_wp
  %so_eof = icmp eq i64 %so_rval, 0
  br i1 %so_eof, label %so_open, label %so_cbody
so_cbody:
  %so_cinext = add i64 %so_ci, 1
  br label %so_copy
so_open:
  %so_dirfd = trunc i64 %x0 to i32
  %so_flags = trunc i64 %x2 to i32
  %so_mode  = trunc i64 %x3 to i32
  %so_fd = call i32 @openat(i32 %so_dirfd, ptr %so_pbuf, i32 %so_flags, i32 %so_mode)
  %so_fd64 = sext i32 %so_fd to i64
  ret i64 %so_fd64
sc_close:
  %sv_fd = trunc i64 %x0 to i32
  call i32 @close(i32 %sv_fd)
  ret i64 0
sc_exit:
  %sx_code = trunc i64 %x0 to i32
  call void @exit(i32 %sx_code)
  unreachable
sc_default:
  ret i64 -1
}

; @reca_alloc(n): bump-allocate n luces. Returns base address.
; aether[{{ALLOC_PTR_ADDR}}]   is the current cursor (next free address).
; aether[{{K_WATERMARK_LUX}}] is the high-water mark (max addr touched).
; Updates BOTH on success.
define i64 @reca_alloc(i64 %n) {
ra_entry:
  %ptr_ap = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 {{ALLOC_PTR_ADDR}}
  %ap = load i64, ptr %ptr_ap
  %ap_new = add i64 %ap, %n
  %overflow = icmp sge i64 %ap_new, {{TOTAL_SIZE}}
  br i1 %overflow, label %ra_halt, label %ra_ok
ra_halt:
  call void @exit(i32 1)
  unreachable
ra_ok:
  store i64 %ap_new, ptr %ptr_ap
  ; Update watermark: max(watermark, ap_new - 1)
  %ptr_wm = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 {{K_WATERMARK_LUX}}
  %wm_old = load i64, ptr %ptr_wm
  %end_addr = sub i64 %ap_new, 1
  %need_update = icmp ugt i64 %end_addr, %wm_old
  br i1 %need_update, label %ra_setwm, label %ra_done
ra_setwm:
  store i64 %end_addr, ptr %ptr_wm
  br label %ra_done
ra_done:
  ret i64 %ap
}

; @reca_create_lux(): allocate a 2-lux Data Lux. Returns its address.
; Kept for backward compat with yaku.re call sites.
define i64 @reca_create_lux() {
rcl_entry:
  %addr = call i64 @reca_alloc(i64 2)
  ret i64 %addr
}

; @reca_link(src, rel, tgt): add lumen src --rel--> tgt.
; Allocates 3-lux lumen block, prepends to src[1] (lumens_head slot).
define void @reca_link(i64 %src, i64 %rel, i64 %tgt) {
rl_entry:
  %src_bad = icmp sle i64 %src, 0
  br i1 %src_bad, label %rl_exit, label %rl_do
rl_do:
  %lumen = call i64 @reca_alloc(i64 3)
  ; lumen[0] = rel
  %p0 = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 %lumen
  store i64 %rel, ptr %p0
  ; lumen[1] = tgt
  %e1 = add i64 %lumen, 1
  %p1 = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 %e1
  store i64 %tgt, ptr %p1
  ; lumen[2] = src[1] (prev head)
  %src_slot1 = add i64 %src, 1
  %p_src1 = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 %src_slot1
  %old_head = load i64, ptr %p_src1
  %e2 = add i64 %lumen, 2
  %p2 = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 %e2
  store i64 %old_head, ptr %p2
  ; src[1] = lumen (new head)
  store i64 %lumen, ptr %p_src1
  br label %rl_exit
rl_exit:
  ret void
}

; @reca_unlink(src, rel, tgt): remove first matching lumen from src[1].
; Lumen memory is abandoned (no free list — bump alloc only).
define void @reca_unlink(i64 %src, i64 %rel, i64 %tgt) {
ru_entry:
  %src_bad = icmp sle i64 %src, 0
  br i1 %src_bad, label %ru_exit, label %ru_init
ru_init:
  ; pna = address of src[1] (lumens_head pointer)
  %src_slot1 = add i64 %src, 1
  br label %ru_loop
ru_loop:
  ; pna phi: starts as src+1, advances to lumen+2 on mismatch
  %pna = phi i64 [ %src_slot1, %ru_init ], [ %pna_next, %ru_continue ]
  %p_pna = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 %pna
  %e = load i64, ptr %p_pna
  %e_zero = icmp eq i64 %e, 0
  br i1 %e_zero, label %ru_exit, label %ru_check_rel
ru_check_rel:
  %p_erel = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 %e
  %e_rel = load i64, ptr %p_erel
  %rel_match = icmp eq i64 %e_rel, %rel
  br i1 %rel_match, label %ru_check_tgt, label %ru_continue
ru_check_tgt:
  %e_p1 = add i64 %e, 1
  %p_etgt = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 %e_p1
  %e_tgt_v = load i64, ptr %p_etgt
  %tgt_match = icmp eq i64 %e_tgt_v, %tgt
  br i1 %tgt_match, label %ru_found, label %ru_continue
ru_found:
  ; splice: pna = lumen[2] (next lumen)
  %e_p2 = add i64 %e, 2
  %p_enxt = getelementptr [{{TOTAL_SIZE}} x i64], ptr @heap, i64 0, i64 %e_p2
  %e_next_v = load i64, ptr %p_enxt
  store i64 %e_next_v, ptr %p_pna
  br label %ru_exit
ru_continue:
  ; advance: pna = address of lumen[2] (its next pointer)
  %pna_next = add i64 %e, 2
  br label %ru_loop
ru_exit:
  ret void
}
