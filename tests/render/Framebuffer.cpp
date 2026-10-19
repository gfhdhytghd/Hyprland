#include <render/Framebuffer.hpp>
#include <helpers/cm/ColorManagement.hpp>
#include <protocols/types/Buffer.hpp>

#include <gtest/gtest.h>

namespace Render {
    class CFramebufferTestTexture : public ITexture {
      public:
        void setTexParameter(GLenum, GLint) override {
            ;
        }
        void allocate(const Vector2D&, uint32_t) override {
            ;
        }
        void update(uint32_t, uint8_t*, uint32_t, const CRegion&) override {
            ;
        }
    };

    class CFramebufferTestFramebuffer : public IFramebuffer {
      public:
        void release() override {
            m_fbAllocated = false;
            m_tex.reset();
            m_size = {};
        }
        bool readPixels(CHLBufferReference, uint32_t, uint32_t, uint32_t, uint32_t) override {
            return false;
        }
        void bind() override {
            ;
        }
        void addStencil(SP<ITexture> texture) override {
            m_stencilTex = texture;
        }

      protected:
        bool internalAlloc(int, int, DRMFormat) override {
            if (!m_tex)
                m_tex = makeShared<CFramebufferTestTexture>();
            return true;
        }
    };

    TEST(Framebuffer, ResizeKeepsImageDescription) {
        CFramebufferTestFramebuffer fb;
        ASSERT_TRUE(fb.alloc(100, 100, DRM_FORMAT_ABGR16161616F));
        fb.setImageDescription(NColorManagement::LINEAR_IMAGE_DESCRIPTION);

        const auto OLD_TEX = fb.getTexture();

        // blur disabled shrinks the blur FB, re-enabling it grows it back
        ASSERT_TRUE(fb.alloc(1, 1, DRM_FORMAT_ABGR16161616F));
        ASSERT_TRUE(fb.alloc(100, 100, DRM_FORMAT_ABGR16161616F));

        ASSERT_NE(fb.getTexture(), OLD_TEX);
        EXPECT_EQ(fb.getTexture()->m_imageDescription, NColorManagement::LINEAR_IMAGE_DESCRIPTION);
        EXPECT_EQ(fb.imageDescription(), NColorManagement::LINEAR_IMAGE_DESCRIPTION);
    }

    TEST(Framebuffer, FormatChangeKeepsImageDescription) {
        CFramebufferTestFramebuffer fb;
        ASSERT_TRUE(fb.alloc(100, 100, DRM_FORMAT_ARGB8888));
        fb.setImageDescription(NColorManagement::DEFAULT_HDR_IMAGE_DESCRIPTION);

        ASSERT_TRUE(fb.alloc(100, 100, DRM_FORMAT_XRGB2101010));

        EXPECT_EQ(fb.imageDescription(), NColorManagement::DEFAULT_HDR_IMAGE_DESCRIPTION);
    }

    TEST(Framebuffer, ReleaseAndReallocKeepsImageDescription) {
        CFramebufferTestFramebuffer fb;
        ASSERT_TRUE(fb.alloc(100, 100, DRM_FORMAT_ARGB8888));
        fb.setImageDescription(NColorManagement::LINEAR_IMAGE_DESCRIPTION);

        fb.release();
        ASSERT_TRUE(fb.alloc(100, 100, DRM_FORMAT_ARGB8888));

        EXPECT_EQ(fb.imageDescription(), NColorManagement::LINEAR_IMAGE_DESCRIPTION);
    }

    TEST(Framebuffer, UndescribedReallocStaysUndescribed) {
        CFramebufferTestFramebuffer fb;
        ASSERT_TRUE(fb.alloc(100, 100, DRM_FORMAT_ARGB8888));
        ASSERT_TRUE(fb.alloc(50, 50, DRM_FORMAT_ARGB8888));

        EXPECT_FALSE(fb.imageDescription());
    }
}
