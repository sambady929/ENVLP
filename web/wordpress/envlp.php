<?php
/**
 * Plugin Name:       ENVLP
 * Description:       Embed the ENVLP symbolic circuit analyser (WebAssembly) in a page with [envlp].
 * Version:           0.1.0
 * Requires at least: 5.0
 * License:           GPL-2.0-or-later
 * License URI:       https://www.gnu.org/licenses/gpl-2.0.html
 *
 * Install
 * -------
 *   1. Build the web app:  bash tools/build_web.sh   (produces web/)
 *   2. Upload the *contents* of web/ to  wp-content/uploads/envlp/
 *      (index.html, app.js, style.css, envlp.js, envlp.wasm, mathjax/,
 *       plotly/, examples/)
 *   3. Put this file in  wp-content/plugins/envlp/envlp.php  and activate it.
 *   4. Add  [envlp]  to any page or post.
 *
 * Attributes:  [envlp height="820" src="/wp-content/uploads/envlp/index.html"]
 */

defined('ABSPATH') || exit;

add_shortcode('envlp', function ($atts) {
    $a = shortcode_atts([
        'height' => '820',
        'src'    => content_url('/uploads/envlp/index.html'),
        'title'  => 'ENVLP symbolic circuit analyser',
    ], $atts, 'envlp');

    return sprintf(
        '<iframe class="envlp-frame" src="%s" title="%s" loading="lazy" ' .
        'style="width:100%%;height:%spx;border:1px solid #e2e6ee;border-radius:8px;' .
        'background:#f4f6fa" allow="fullscreen"></iframe>',
        esc_url($a['src']),
        esc_attr($a['title']),
        (int) $a['height']
    );
});

/**
 * Some hosts need a nudge to serve .wasm with the right type; a wrong MIME type
 * stops WebAssembly.instantiateStreaming from working. Add it for our file only.
 */
add_filter('mime_types', function ($mimes) {
    $mimes['wasm'] = 'application/wasm';
    return $mimes;
});
