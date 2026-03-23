.. _gdal_vector_concave_hull:

================================================================================
``gdal vector concave-hull``
================================================================================

.. versionadded:: 3.13

.. only:: html

    Compute a concave hull around geometries of a vector dataset.

.. Index:: gdal vector concave-hull

Synopsis
--------

.. program-output:: gdal vector concave-hull --help-doc

Description
-----------

:program:`gdal vector concave-hull` computes the smallest concave polygon that
covers the input geometry. Generally the result will be a Polygon, but for
degenerate inputs it may be a LineString or a Point.

This command can also be used as a step of :ref:`gdal_vector_pipeline`.

.. GDALG output (on-the-fly / streamed dataset)
.. --------------------------------------------

.. include:: gdal_cli_include/gdalg_vector_compatible.rst

.. note:: This command requires a GDAL build against the GEOS library.

Program-Specific Options
------------------------

.. option:: --allow-holes

   Allow result polygons to contain holes.

.. option:: --ratio <RATIO>

   Ratio controlling the concavity of the geometry. A value of 1 produces the convex hull; decreasing values of the ratio generally produce polygons of increasing concavity.


Standard Options
----------------

.. collapse:: Details

    .. include:: gdal_options/append_vector.rst

    .. include:: gdal_options/active_layer.rst

    .. include:: gdal_options/active_geometry.rst

    .. include:: gdal_options/co_vector.rst

    .. include:: gdal_options/if.rst

    .. include:: gdal_options/input_layer.rst

    .. include:: gdal_options/lco.rst
       
    .. include:: gdal_options/oo.rst

    .. include:: gdal_options/of_vector.rst

    .. include:: gdal_options/output_layer.rst

    .. include:: gdal_options/output_oo.rst

    .. include:: gdal_options/overwrite.rst

    .. include:: gdal_options/overwrite_layer.rst

    .. include:: gdal_options/skip_errors.rst

    .. include:: gdal_options/update.rst

    .. include:: gdal_options/upsert.rst


Examples
--------

.. example::
   :title: Compute the concave hull of city locations in each country


   Here the `Natural Earth Populated Places dataset <https://www.naturalearthdata.com/downloads/10m-cultural-vectors/10m-populated-places/>`_ is used. To ensure that a polygonal hull is generated for each country,
   the input points are first buffered by a small amount. Otherwise, countries having only one or two cities would end
   up with a point or line geometry.

   .. code-block:: bash

        gdal vector pipeline ! \
            read ne_10m_populated_places.shp ! \
            buffer 0.1 ! \
            combine --group-by ADM0_A3 ! \
            concave-hull --ratio 0.5 ! \
            write country_city_hulls.gpkg
